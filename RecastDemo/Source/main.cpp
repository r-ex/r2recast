//
// Copyright (c) 2009-2010 Mikko Mononen memon@inside.org
//
// This software is provided 'as-is', without any express or implied
// warranty.  In no event will the authors be held liable for any damages
// arising from the use of this software.
// Permission is granted to anyone to use this software for any purpose,
// including commercial applications, and to alter it and redistribute it
// freely, subject to the following restrictions:
// 1. The origin of this software must not be misrepresented; you must not
//    claim that you wrote the original software. If you use this software
//    in a product, an acknowledgment in the product documentation would be
//    appreciated but is not required.
// 2. Altered source versions must be plainly marked as such, and must not be
//    misrepresented as being the original software.
// 3. This notice may not be removed or altered from any source distribution.
//

#include <cstdio>
#define _USE_MATH_DEFINES
#include <cmath>

#include "SDL.h"
#include "SDL_syswm.h"
#include "SDL_opengl.h"
#ifdef __APPLE__
#	include <OpenGL/glu.h>
#else
#	include <GL/glu.h>
#endif

#include <vector>
#include <string>

#include "imgui.h"
#include "imguiRenderGL.h"

#include "Recast.h"
#include "RecastDebugDraw.h"
#include "InputGeom.h"
#include "TestCase.h"
#include "Filelist.h"
#include "Sample_SoloMesh.h"
#include "Sample_TileMesh.h"
#include "Sample_TempObstacles.h"
#include "Sample_Debug.h"

#include "RecastAlloc.h"


#ifdef WIN32
#	define snprintf _snprintf
#	define putenv _putenv
#include "commdlg.h"

#endif

using std::string;
using std::vector;

struct SampleItem
{
	Sample* (*create)();
	const string name;
};
Sample* createSolo() { return new Sample_SoloMesh(); }
Sample* createTile() { return new Sample_TileMesh(); }
Sample* createTempObstacle() { return new Sample_TempObstacles(); }
Sample* createDebug() { return new Sample_Debug(); }
static SampleItem g_samples[] =
{
	{ createSolo, "Solo Mesh" },
	{ createTile, "Tile Mesh" },
	{ createTempObstacle, "Temp Obstacles" },
};
static const int g_nsamples = sizeof(g_samples) / sizeof(SampleItem);

void save_ply(std::vector<float>& pts,std::vector<int>& colors,rcIntArray& tris)
{
	static int counter = 0;
	char fname[255];
	sprintf(fname, "out_%d.ply", counter);
	counter++;
	auto f = fopen(fname, "wb");
	fprintf(f,
R"(ply
format ascii 1.0
element vertex %d
property float x
property float y
property float z
property uchar red
property uchar green
property uchar blue
element face %d
property list uchar int vertex_index
end_header
)",pts.size()/3,tris.size()/3);

	for (size_t i = 0; i < pts.size(); i+=3)
	{
		auto c = colors[i / 3];
		fprintf(f, "%g %g %g %d %d %d\n", pts[i], pts[i + 1], pts[i + 2], c & 0xff, (c >> 8) & 0xff, (c >> 16) & 0xff);
	}
	for (size_t i = 0; i < tris.size(); i += 3)
	{
		fprintf(f, "3 %d %d %d\n", tris[i], tris[i + 1], tris[i + 2]);
	}
	
	fclose(f);
}
float area2(const float* a, const float* b, const float* c)
{
	return (b[0] - a[0]) * (c[1] - a[1]) - (c[0] - a[0]) * (b[1] - a[1]);
}
void convex_hull(std::vector<float>& pts, std::vector<int>& hull)
{
	int pt_count = pts.size() / 3;
	int cur_pt = 0;
	float min_x = pts[0];
	for(size_t i=0;i<pt_count;i++)
		if (pts[i * 3] < min_x)
		{
			min_x = pts[i * 3];
			cur_pt = i;
		}

	
	int point_on_hull = cur_pt;
	int endpoint = 0;
	do
	{
		hull.push_back(point_on_hull);
		endpoint = (point_on_hull + 1) % pt_count;
		for (int i = 0; i < pt_count; i++)
		{
			if (area2(&pts[point_on_hull*3], &pts[i*3], &pts[endpoint*3]) > 0) //reverse this comparison for flipped hull direction
				endpoint = i;
		}
		point_on_hull = endpoint;
	} while (endpoint != hull[0]);
}
float frand()
{
	return rand() / (float)RAND_MAX;
}
void generate_points(float* pts, int count, float dx, float dy, float dz)
{
	for (int i = 0; i < count; i++)
	{
		pts[i * 3+0] = frand()*dx * 2 - dx;
		pts[i * 3+1] = frand()*dy * 2 - dy;
		pts[i * 3+2] = frand()*dz * 2 - dz;
	}
}

void do_auto_load(const char* path, BuildContext& ctx,Sample* sample,InputGeom*& geom, string& meshName,bool& tf2_transforms)
{
	string geom_path = std::string(path);
	meshName = geom_path.substr(geom_path.rfind("\\") + 1);
	geom = new InputGeom;
	if (!geom->load(&ctx, geom_path, tf2_transforms))
	{
		delete geom;
		geom = 0;

		// Destroy the sample if it already had geometry loaded, as we've just deleted it!
		/*if (sample && sample->getInputGeom())
		{
			delete sample;
			sample = 0;
		}*/
		ctx.dumpLog("Geom load log %s:", meshName.c_str());
	}
	if (sample && geom)
	{
		sample->handleMeshChanged(geom);
		sample->m_model_name = meshName.substr(0, meshName.size() - 4);
	}
}

void update_camera(const float* bmin, const float* bmax,float* cameraPos,float* cameraEulers,float& camr)
{
	// Reset camera and fog to match the mesh bounds.
	if (bmin && bmax)
	{
		camr = sqrtf(rcSqr(bmax[0] - bmin[0]) +
			rcSqr(bmax[1] - bmin[1]) +
			rcSqr(bmax[2] - bmin[2])) / 2;
		cameraPos[0] = (bmax[0] + bmin[0]) / 2 + camr;
		cameraPos[1] = (bmax[1] + bmin[1]) / 2 + camr;
		cameraPos[2] = (bmax[2] + bmin[2]) / 2 + camr;
		camr *= 3;
	}
	cameraEulers[0] = 45;
	cameraEulers[1] = -125;
	glFogf(GL_FOG_START, camr * 0.1f);
	glFogf(GL_FOG_END, camr * 1.25f);
}
#if 1
int main(int argc, char** argv)
#else
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
//just quick tests for stuff



extern void delaunayHull(rcContext* ctx, const int npts, const float* pts,
	const int nhull, const int* hull,
	rcIntArray& tris, rcIntArray& edges);

int main_test_delaunay(int /*argc*/, char** /*argv*/)
{
	rcContext ctx;
	std::vector<float> pts(25*3);
	std::vector<int> colors(pts.size() / 3, 0xffffffff);
	generate_points(pts.data(), pts.size()/3, 10, 10, 2);
	

	std::vector<int> hull;
	convex_hull(pts,hull);

	for (auto h : hull)
		colors[h] = 0xffff0000;

	rcIntArray tris;
	save_ply(pts, colors, tris);

	rcIntArray edges;
	delaunayHull(&ctx, pts.size()/3, pts.data(), hull.size(), hull.data(), tris, edges);
	save_ply(pts, colors, tris);
	return 0;
}
void compact_tris(rcIntArray& tris)
{
	int j = 3;
	for (int i = 4; i < tris.size(); i++)
	{
		if (i % 4 == 3) continue;
		tris[j] = tris[i];
		j++;
	}
	tris.resize(j);
}
int main(int argc, char** argv)
{
	srand(17);
	rcContext ctx;
	std::vector<float> pts(8 * 3);
	std::vector<int> colors(pts.size() / 3, 0xffffffff);
	generate_points(pts.data(), pts.size() / 3, 10, 10, 10);


	std::vector<int> hull;
	convex_hull(pts, hull);

	for (auto h : hull)
		colors[h] = 0xffff0000;

	rcIntArray tris;
	//save_ply(pts, colors, tris);

	rcIntArray edges;
	delaunayHull(&ctx, pts.size() / 3, pts.data(), hull.size(), hull.data(), tris, edges);
	compact_tris(tris);
	save_ply(pts, colors, tris);
	int tri_count = tris.size() / 3;
	std::vector<unsigned char> areas;
	areas.resize(tri_count);
	for (int i = 0; i < tri_count; i++)
		areas[i] = i;


	float bmin[3];
	float bmax[3];
	rcCalcBounds(pts.data(), pts.size()/3, bmin, bmax);

	float cellSize = .05f;
	float cellHeight = .05f;

	int width;
	int height;

	rcCalcGridSize(bmin, bmax, cellSize, &width, &height);

	rcHeightfield solid;
	rcCreateHeightfield(&ctx, solid, width, height, bmin, bmax, cellSize, cellHeight);

	int flagMergeThr = 1;

	rcRasterizeTriangles(&ctx, pts.data(), pts.size() / 3, tris.data(), areas.data(), tri_count, solid, flagMergeThr);

	std::vector<unsigned char> img_data(width*height);
	float zdelta = bmax[2] - bmin[2];
	for(int x=0;x<width;x++)
		for (int y = 0; y < height; y++)
		{
			auto s = solid.spans[x + y * width];
			if (s )
			{
#if 1
				img_data[x + y * width] = s->area*(235/ (float)tri_count) +20;
#else
				img_data[x + y * width]=((s->smax*cellHeight)/zdelta)*255;
#endif
				//img_data[x + y * width] = 255;
			}
		}

	stbi_write_png("hmap.png", width, height, 1, img_data.data(), width);
	
	return 0;
}
int not_main(int argc, char** argv)
#endif
{
	const char* auto_load =nullptr;
	if (argc > 1)
	{
		auto_load = argv[1];
	}
	// Init SDL
	if (SDL_Init(SDL_INIT_EVERYTHING) != 0)
	{
		printf("Could not initialise SDL.\nError: %s\n", SDL_GetError());
		return -1;
	}

	// Enable depth buffer.
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
	SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
	
	// Set color channel depth.
	SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
	
	// 4x MSAA.
	SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
	SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 4);

	SDL_DisplayMode displayMode;
	SDL_GetCurrentDisplayMode(0, &displayMode);

	bool presentationMode = false;
	Uint32 flags = SDL_WINDOW_OPENGL | SDL_RENDERER_PRESENTVSYNC;
	int width;
	int height;
	if (presentationMode)
	{
		// Create a fullscreen window at the native resolution.
		width = displayMode.w;
		height = displayMode.h;
		flags |= SDL_WINDOW_FULLSCREEN;
	}
	else
	{
		float aspect = 16.0f / 9.0f;
		width = rcMin(displayMode.w, (int)(displayMode.h * aspect)) - 80;
		height = displayMode.h - 80;
	}
	
	SDL_Window* window;
	SDL_Renderer* renderer;
	int errorCode = SDL_CreateWindowAndRenderer(width, height, flags, &window, &renderer);

	if (errorCode != 0 || !window || !renderer)
	{
		printf("Could not initialise SDL opengl\nError: %s\n", SDL_GetError());
		return -1;
	}

	SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
	SDL_GL_CreateContext(window);

	if (!imguiRenderGLInit("DroidSans.ttf"))
	{
		printf("Could not init GUI renderer.\n");
		SDL_Quit();
		return -1;
	}
	
	float t = 0.0f;
	float timeAcc = 0.0f;
	Uint32 prevFrameTime = SDL_GetTicks();
	int mousePos[2] = {0, 0};
	int origMousePos[2] = {0, 0}; // Used to compute mouse movement totals across frames.
	
	float cameraEulers[] = {45, 45};
	float cameraPos[] = {0, 0, 0};
	float camr = 1000;
	float origCameraEulers[] = {0, 0}; // Used to compute rotational changes across frames.
	
	float moveFront = 0.0f, moveBack = 0.0f, moveLeft = 0.0f, moveRight = 0.0f, moveUp = 0.0f, moveDown = 0.0f;
	
	float scrollZoom = 0;
	bool rotate = false;
	bool movedDuringRotate = false;
	float rayStart[3];
	float rayEnd[3];
	bool mouseOverMenu = false;
	
	bool showMenu = !presentationMode;
	bool showLog = false;
	bool showTools = true;
	bool showLevels = false;
	bool showSample = false;
	bool showTestCases = false;

	// Window scroll positions.
	int propScroll = 0;
	int logScroll = 0;
	int toolsScroll = 0;
	
	string sampleName = "Choose Sample...";
	
	vector<string> files;
	const string meshesFolder = "Meshes";
	string meshName = "Choose Mesh...";
	
	float markerPosition[3] = {0, 0, 0};
	bool markerPositionSet = false;
	
	InputGeom* geom = 0;
	Sample* sample = 0;
	bool tf2_transforms = false;

	const string testCasesFolder = "TestCases";
	TestCase* test = 0;

	BuildContext ctx;
	
	//Load tiled sample

	sample = g_samples[1].create();
	sampleName = g_samples[1].name;
	sample->is_tf2 = &tf2_transforms;
	sample->setContext(&ctx);
	if (geom)
	{
		sample->handleMeshChanged(geom);
	}
	if (auto_load)
	{
		do_auto_load(auto_load, ctx, sample, geom, meshName, tf2_transforms);
		if (geom || sample)
		{
			const float* bmin = 0;
			const float* bmax = 0;
			if (geom)
			{
				bmin = geom->getNavMeshBoundsMin();
				bmax = geom->getNavMeshBoundsMax();
			}
			update_camera(bmin, bmax, cameraPos, cameraEulers, camr);
		}
		if (argc > 2)
		{
			auto ts = dynamic_cast<Sample_TileMesh*>(sample);
			ts->build_n_SaveAllHulls();
			return 0;
		}
	}
	// Fog.
	float fogColor[4] = { 0.32f, 0.31f, 0.30f, 1.0f };
	glEnable(GL_FOG);
	glFogi(GL_FOG_MODE, GL_LINEAR);
	glFogf(GL_FOG_START, camr * 0.1f);
	glFogf(GL_FOG_END, camr * 1.25f);
	glFogfv(GL_FOG_COLOR, fogColor);
	
	glEnable(GL_CULL_FACE);
	glDepthFunc(GL_LEQUAL);

	


	bool done = false;
	while(!done)
	{
		// Handle input events.
		int mouseScroll = 0;
		bool processHitTest = false;
		bool processHitTestShift = false;
		
		SDL_Event event;
		
		while (SDL_PollEvent(&event))
		{
			switch (event.type)
			{
				case SDL_KEYDOWN:
					// Handle any key presses here.
					if (event.key.keysym.sym == SDLK_ESCAPE)
					{
						done = true;
					}
					else if (event.key.keysym.sym == SDLK_t)
					{
						showLevels = false;
						showSample = false;
						showTestCases = true;
						scanDirectory(testCasesFolder, ".txt", files);
					}
					else if (event.key.keysym.sym == SDLK_TAB)
					{
						showMenu = !showMenu;
					}
					else if (event.key.keysym.sym == SDLK_SPACE)
					{
						if (sample)
							sample->handleToggle();
					}
					else if (event.key.keysym.sym == SDLK_1)
					{
						if (sample)
							sample->handleStep();
					}
					else if (event.key.keysym.sym == SDLK_9)
					{
						if (sample && geom)
						{
							string savePath = meshesFolder + "/";
							BuildSettings settings;
							memset(&settings, 0, sizeof(settings));

							rcVcopy(settings.navMeshBMin, geom->getNavMeshBoundsMin());
							rcVcopy(settings.navMeshBMax, geom->getNavMeshBoundsMax());

							sample->collectSettings(settings);

							geom->saveGeomSet(&settings);
						}
					}
					break;
				
				case SDL_MOUSEWHEEL:
					if (event.wheel.y < 0)
					{
						// wheel down
						if (mouseOverMenu)
						{
							mouseScroll++;
						}
						else
						{
							scrollZoom += 1.0f;
						}
					}
					else
					{
						if (mouseOverMenu)
						{
							mouseScroll--;
						}
						else
						{
							scrollZoom -= 1.0f;
						}
					}
					break;
				case SDL_MOUSEBUTTONDOWN:
					if (event.button.button == SDL_BUTTON_RIGHT)
					{
						if (!mouseOverMenu)
						{
							// Rotate view
							rotate = true;
							movedDuringRotate = false;
							origMousePos[0] = mousePos[0];
							origMousePos[1] = mousePos[1];
							origCameraEulers[0] = cameraEulers[0];
							origCameraEulers[1] = cameraEulers[1];
						}
					}
					break;
					
				case SDL_MOUSEBUTTONUP:
					// Handle mouse clicks here.
					if (event.button.button == SDL_BUTTON_RIGHT)
					{
						rotate = false;
						if (!mouseOverMenu)
						{
							if (!movedDuringRotate)
							{
								processHitTest = true;
								processHitTestShift = true;
							}
						}
					}
					else if (event.button.button == SDL_BUTTON_LEFT)
					{
						if (!mouseOverMenu)
						{
							processHitTest = true;
							processHitTestShift = (SDL_GetModState() & KMOD_SHIFT) ? true : false;
						}
					}
					
					break;
					
				case SDL_MOUSEMOTION:
					mousePos[0] = event.motion.x;
					mousePos[1] = height-1 - event.motion.y;
					
					if (rotate)
					{
						int dx = mousePos[0] - origMousePos[0];
						int dy = mousePos[1] - origMousePos[1];
						cameraEulers[0] = origCameraEulers[0] - dy * 0.25f;
						cameraEulers[1] = origCameraEulers[1] + dx * 0.25f;
						if (dx * dx + dy * dy > 3 * 3)
						{
							movedDuringRotate = true;
						}
					}
					break;
					
				case SDL_QUIT:
					done = true;
					break;
					
				default:
					break;
			}
		}

		unsigned char mouseButtonMask = 0;
		if (SDL_GetMouseState(0, 0) & SDL_BUTTON_LMASK)
			mouseButtonMask |= IMGUI_MBUT_LEFT;
		if (SDL_GetMouseState(0, 0) & SDL_BUTTON_RMASK)
			mouseButtonMask |= IMGUI_MBUT_RIGHT;
		
		Uint32 time = SDL_GetTicks();
		float dt = (time - prevFrameTime) / 1000.0f;
		prevFrameTime = time;
		
		t += dt;

		// Hit test mesh.
		if (processHitTest && geom && sample)
		{
			float hitTime;
			bool hit = geom->raycastMesh(rayStart, rayEnd, hitTime);
			
			if (hit)
			{
				if (SDL_GetModState() & KMOD_CTRL)
				{
					// Marker
					markerPositionSet = true;
					markerPosition[0] = rayStart[0] + (rayEnd[0] - rayStart[0]) * hitTime;
					markerPosition[1] = rayStart[1] + (rayEnd[1] - rayStart[1]) * hitTime;
					markerPosition[2] = rayStart[2] + (rayEnd[2] - rayStart[2]) * hitTime;
				}
				else
				{
					float pos[3];
					pos[0] = rayStart[0] + (rayEnd[0] - rayStart[0]) * hitTime;
					pos[1] = rayStart[1] + (rayEnd[1] - rayStart[1]) * hitTime;
					pos[2] = rayStart[2] + (rayEnd[2] - rayStart[2]) * hitTime;
					sample->handleClick(rayStart, pos, processHitTestShift);
				}
			}
			else
			{
				if (SDL_GetModState() & KMOD_CTRL)
				{
					// Marker
					markerPositionSet = false;
				}
			}
		}
		
		// Update sample simulation.
		const float SIM_RATE = 20;
		const float DELTA_TIME = 1.0f / SIM_RATE;
		timeAcc = rcClamp(timeAcc + dt, -1.0f, 1.0f);
		int simIter = 0;
		while (timeAcc > DELTA_TIME)
		{
			timeAcc -= DELTA_TIME;
			if (simIter < 5 && sample)
			{
				sample->handleUpdate(DELTA_TIME);
			}
			simIter++;
		}

		// Clamp the framerate so that we do not hog all the CPU.
		const float MIN_FRAME_TIME = 1.0f / 40.0f;
		if (dt < MIN_FRAME_TIME)
		{
			int ms = (int)((MIN_FRAME_TIME - dt) * 1000.0f);
			if (ms > 10) ms = 10;
			if (ms >= 0) SDL_Delay(ms);
		}
		
		// Set the viewport.
		glViewport(0, 0, width, height);
		GLint viewport[4];
		glGetIntegerv(GL_VIEWPORT, viewport);
		
		// Clear the screen
		glClearColor(0.3f, 0.3f, 0.32f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		glDisable(GL_TEXTURE_2D);
		glEnable(GL_DEPTH_TEST);
		
		// Compute the projection matrix.
		glMatrixMode(GL_PROJECTION);
		glLoadIdentity();
		gluPerspective(50.0f, (float)width/(float)height, 1.0f, camr);
		GLdouble projectionMatrix[16];
		glGetDoublev(GL_PROJECTION_MATRIX, projectionMatrix);
		
		// Compute the modelview matrix.
		glMatrixMode(GL_MODELVIEW);
		glLoadIdentity();
		glRotatef(cameraEulers[0], 1, 0, 0);
		glRotatef(cameraEulers[1], 0, 1, 0);
		float mXZY_to_XYZ[16] =
		{
			1,0,0,0,
			0,0,-1,0, //tbh not sure why this is needed, the tri flips again? something is very stupid...
			0,1,0,0,
			0,0,0,1
		};
		glMultMatrixf(mXZY_to_XYZ);
		glTranslatef(-cameraPos[0], -cameraPos[1], -cameraPos[2]);
		GLdouble modelviewMatrix[16];
		glGetDoublev(GL_MODELVIEW_MATRIX, modelviewMatrix);
		
		// Get hit ray position and direction.
		GLdouble x, y, z;
		gluUnProject(mousePos[0], mousePos[1], 0.0f, modelviewMatrix, projectionMatrix, viewport, &x, &y, &z);
		rayStart[0] = (float)x;
		rayStart[1] = (float)y;
		rayStart[2] = (float)z;
		gluUnProject(mousePos[0], mousePos[1], 1.0f, modelviewMatrix, projectionMatrix, viewport, &x, &y, &z);
		rayEnd[0] = (float)x;
		rayEnd[1] = (float)y;
		rayEnd[2] = (float)z;
		
		// Handle keyboard movement.
		const Uint8* keystate = SDL_GetKeyboardState(NULL);
		moveFront	= rcClamp(moveFront	+ dt * 4 * ((keystate[SDL_SCANCODE_W] || keystate[SDL_SCANCODE_UP		]) ? 1 : -1), 0.0f, 1.0f);
		moveLeft	= rcClamp(moveLeft	+ dt * 4 * ((keystate[SDL_SCANCODE_A] || keystate[SDL_SCANCODE_LEFT		]) ? 1 : -1), 0.0f, 1.0f);
		moveBack	= rcClamp(moveBack	+ dt * 4 * ((keystate[SDL_SCANCODE_S] || keystate[SDL_SCANCODE_DOWN		]) ? 1 : -1), 0.0f, 1.0f);
		moveRight	= rcClamp(moveRight	+ dt * 4 * ((keystate[SDL_SCANCODE_D] || keystate[SDL_SCANCODE_RIGHT	]) ? 1 : -1), 0.0f, 1.0f);
		moveUp		= rcClamp(moveUp	+ dt * 4 * ((keystate[SDL_SCANCODE_Q] || keystate[SDL_SCANCODE_PAGEUP	]) ? 1 : -1), 0.0f, 1.0f);
		moveDown	= rcClamp(moveDown	+ dt * 4 * ((keystate[SDL_SCANCODE_E] || keystate[SDL_SCANCODE_PAGEDOWN	]) ? 1 : -1), 0.0f, 1.0f);
		
		float keybSpeed = 8800.0f;
		if (SDL_GetModState() & KMOD_SHIFT)
		{
			keybSpeed *= 2.0f;
		}
		
		float movex = (moveRight - moveLeft) * keybSpeed * dt;
		float movey = (moveBack - moveFront) * keybSpeed * dt + scrollZoom * 2.0f;
		scrollZoom = 0;
		
		cameraPos[0] += movex * (float)modelviewMatrix[0];
		cameraPos[1] += movex * (float)modelviewMatrix[4];
		cameraPos[2] += movex * (float)modelviewMatrix[8];
		
		cameraPos[0] += movey * (float)modelviewMatrix[2];
		cameraPos[1] += movey * (float)modelviewMatrix[6];
		cameraPos[2] += movey * (float)modelviewMatrix[10];

		cameraPos[1] += (moveUp - moveDown) * keybSpeed * dt;

		glEnable(GL_FOG);

		if (sample)
			sample->handleRender();
		if (test)
			test->handleRender();
		
		glDisable(GL_FOG);
		
		// Render GUI
		glDisable(GL_DEPTH_TEST);
		glMatrixMode(GL_PROJECTION);
		glLoadIdentity();
		gluOrtho2D(0, width, 0, height);
		glMatrixMode(GL_MODELVIEW);
		glLoadIdentity();
		
		mouseOverMenu = false;
		
		imguiBeginFrame(mousePos[0], mousePos[1], mouseButtonMask, mouseScroll);
		
		if (sample)
		{
			sample->handleRenderOverlay((double*)projectionMatrix, (double*)modelviewMatrix, (int*)viewport);
		}
		if (test)
		{
			if (test->handleRenderOverlay((double*)projectionMatrix, (double*)modelviewMatrix, (int*)viewport))
				mouseOverMenu = true;
		}

		// Help text.
		if (showMenu)
		{
			const char msg[] = "W/S/A/D: Move  RMB: Rotate";
			imguiDrawText(280, height-20, IMGUI_ALIGN_LEFT, msg, imguiRGBA(255,255,255,128));
		}
		string geom_path;
		if (showMenu)
		{
			if (imguiBeginScrollArea("Properties", width-250-10, 10, 250, height-20, &propScroll))
				mouseOverMenu = true;

			if (imguiCheck("Show Log", showLog))
				showLog = !showLog;
			if (imguiCheck("Show Tools", showTools))
				showTools = !showTools;

			imguiSeparator();
			imguiLabel("Sample");
			if (imguiButton(sampleName.c_str()))
			{
				if (showSample)
				{
					showSample = false;
				}
				else
				{
					showSample = true;
					showLevels = false;
					showTestCases = false;
				}
			}
			
			imguiSeparator();
			//if (imguiCheck("Import/Export TF2", tf2_transforms, true))
			//	tf2_transforms = !tf2_transforms;
			imguiLabel("Input Mesh");
			if (imguiButton("Load mesh..."))
			{
				char szFile[260];
				OPENFILENAMEA diag = { 0 };
				diag.lStructSize = sizeof(diag);

				SDL_SysWMinfo sdlinfo;
				SDL_version sdlver;
				SDL_VERSION(&sdlver);
				sdlinfo.version = sdlver;
				SDL_GetWindowWMInfo(window, &sdlinfo);

				diag.hwndOwner = sdlinfo.info.win.window;

				diag.lpstrFile = szFile;
				diag.lpstrFile[0] = 0;
				diag.nMaxFile = sizeof(szFile);
				diag.lpstrFilter = "OBJ\0*.obj\0Ply\0*.ply\0All\0*.*\0"; //TODO: BSP\0*.bsp\0
				diag.nFilterIndex = 1;
				diag.lpstrFileTitle = NULL;
				diag.nMaxFileTitle = 0;
				diag.lpstrInitialDir = NULL;
				diag.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;

				if (GetOpenFileNameA(&diag))
				{
					geom_path = std::string(szFile);
					meshName = geom_path.substr(geom_path.rfind("\\")+1);
				}
			}
			if (imguiButton(meshName.c_str()))
			{
				if (showLevels)
				{
					showLevels = false;
				}
				else
				{
					showSample = false;
					showTestCases = false;
					showLevels = true;
					scanDirectory(meshesFolder, ".obj", files);
					scanDirectoryAppend(meshesFolder, ".gset", files);
					scanDirectoryAppend(meshesFolder, ".ply", files);
				}
			}
			if (geom)
			{
				char text[64];
				snprintf(text, 64, "Verts: %.1fk  Tris: %.1fk",
						 geom->getMesh()->getVertCount()/1000.0f,
						 geom->getMesh()->getTriCount()/1000.0f);
				imguiValue(text);
			}
			imguiSeparator();

			if (geom && sample)
			{
				imguiSeparatorLine();
				
				sample->handleSettings();

				if (imguiButton("Build"))
				{
					ctx.resetLog();
					if (!sample->handleBuild())
					{
						showLog = true;
						logScroll = 0;
					}
					ctx.dumpLog("Build log %s:", meshName.c_str());
					
					// Clear test.
					delete test;
					test = 0;
				}

				imguiSeparator();
			}
			
			if (sample)
			{
				imguiSeparatorLine();
				sample->handleDebugMode();
			}

			imguiEndScrollArea();
		}
		
		// Sample selection dialog.
		if (showSample)
		{
			static int levelScroll = 0;
			if (imguiBeginScrollArea("Choose Sample", width-10-250-10-200, height-10-250, 200, 250, &levelScroll))
				mouseOverMenu = true;

			Sample* newSample = 0;
			for (int i = 0; i < g_nsamples; ++i)
			{
				if (imguiItem(g_samples[i].name.c_str()))
				{
					newSample = g_samples[i].create();
					if (newSample)
					{
						sampleName = g_samples[i].name;
						newSample->is_tf2 = &tf2_transforms;
					}
				}
			}
			if (newSample)
			{
				delete sample;
				sample = newSample;
				sample->setContext(&ctx);
				if (geom)
				{
					sample->handleMeshChanged(geom);
				}
				showSample = false;
			}

			if (geom || sample)
			{
				const float* bmin = 0;
				const float* bmax = 0;
				if (geom)
				{
					bmin = geom->getNavMeshBoundsMin();
					bmax = geom->getNavMeshBoundsMax();
				}
				// Reset camera and fog to match the mesh bounds.
				update_camera(bmin, bmax, cameraPos, cameraEulers, camr);
			}
			
			imguiEndScrollArea();
		}

		// Level selection dialog.
		if (showLevels)
		{
			static int levelScroll = 0;
			if (imguiBeginScrollArea("Choose Level", width - 10 - 250 - 10 - 200, height - 10 - 450, 200, 450, &levelScroll))
				mouseOverMenu = true;
			
			vector<string>::const_iterator fileIter = files.begin();
			vector<string>::const_iterator filesEnd = files.end();
			vector<string>::const_iterator levelToLoad = filesEnd;
			for (; fileIter != filesEnd; ++fileIter)
			{
				if (imguiItem(fileIter->c_str()))
				{
					levelToLoad = fileIter;
				}
			}
			
			if (levelToLoad != filesEnd)
			{
				meshName = *levelToLoad;
				showLevels = false;
				
				delete geom;
				geom = 0;
				
				geom_path= meshesFolder + "/" + meshName;
			}
			
			imguiEndScrollArea();
			
		}
		if (!geom_path.empty())
		{
			geom = new InputGeom;
			if (!geom->load(&ctx, geom_path, tf2_transforms))
			{
				delete geom;
				geom = 0;

				// Destroy the sample if it already had geometry loaded, as we've just deleted it!
				if (sample && sample->getInputGeom())
				{
					delete sample;
					sample = 0;
				}

				showLog = true;
				logScroll = 0;
				ctx.dumpLog("Geom load log %s:", meshName.c_str());
			}
			if (sample && geom)
			{
				sample->handleMeshChanged(geom);
				sample->m_model_name = meshName.substr(0, meshName.size() - 4);
			}

			if (geom || sample)
			{
				const float* bmin = 0;
				const float* bmax = 0;
				if (geom)
				{
					bmin = geom->getNavMeshBoundsMin();
					bmax = geom->getNavMeshBoundsMax();
				}
				// Reset camera and fog to match the mesh bounds.
				update_camera(bmin, bmax, cameraPos, cameraEulers, camr);
			}
		}
		// Test cases
		if (showTestCases)
		{
			static int testScroll = 0;
			if (imguiBeginScrollArea("Choose Test To Run", width-10-250-10-200, height-10-450, 200, 450, &testScroll))
				mouseOverMenu = true;

			vector<string>::const_iterator fileIter = files.begin();
			vector<string>::const_iterator filesEnd = files.end();
			vector<string>::const_iterator testToLoad = filesEnd;
			for (; fileIter != filesEnd; ++fileIter)
			{
				if (imguiItem(fileIter->c_str()))
				{
					testToLoad = fileIter;
				}
			}
			
			if (testToLoad != filesEnd)
			{
				string path = testCasesFolder + "/" + *testToLoad;
				test = new TestCase;
				if (test)
				{
					// Load the test.
					if (!test->load(path))
					{
						delete test;
						test = 0;
					}

					// Create sample
					Sample* newSample = 0;
					for (int i = 0; i < g_nsamples; ++i)
					{
						if (g_samples[i].name == test->getSampleName())
						{
							newSample = g_samples[i].create();
							if (newSample)
							{
								sampleName = g_samples[i].name;
								newSample->is_tf2 = &tf2_transforms;
							}
						}
					}

					delete sample;
					sample = newSample;

					if (sample)
					{
						sample->setContext(&ctx);
						showSample = false;
					}

					// Load geom.
					meshName = test->getGeomFileName();
					
					
					path = meshesFolder + "/" + meshName;
					
					delete geom;
					geom = new InputGeom;
					if (!geom || !geom->load(&ctx, path,tf2_transforms))
					{
						delete geom;
						geom = 0;
						delete sample;
						sample = 0;
						showLog = true;
						logScroll = 0;
						ctx.dumpLog("Geom load log %s:", meshName.c_str());
					}
					if (sample && geom)
					{
						sample->handleMeshChanged(geom);
						sample->m_model_name = meshName.substr(0, meshName.size() - 4);
					}

					// This will ensure that tile & poly bits are updated in tiled sample.
					if (sample)
						sample->handleSettings();

					ctx.resetLog();
					if (sample && !sample->handleBuild())
					{
						ctx.dumpLog("Build log %s:", meshName.c_str());
					}
					
					if (geom || sample)
					{
						const float* bmin = 0;
						const float* bmax = 0;
						if (geom)
						{
							bmin = geom->getNavMeshBoundsMin();
							bmax = geom->getNavMeshBoundsMax();
						}
						// Reset camera and fog to match the mesh bounds.
						update_camera(bmin, bmax, cameraPos, cameraEulers, camr);
					}
					
					// Do the tests.
					if (sample)
						test->doTests(sample->getNavMesh(), sample->getNavMeshQuery());
				}
			}				
				
			imguiEndScrollArea();
		}

		
		// Log
		if (showLog && showMenu)
		{
			if (imguiBeginScrollArea("Log", 250 + 20, 10, width - 300 - 250, 200, &logScroll))
				mouseOverMenu = true;
			for (int i = 0; i < ctx.getLogCount(); ++i)
				imguiLabel(ctx.getLogText(i));
			imguiEndScrollArea();
		}
		
		// Left column tools menu
		if (!showTestCases && showTools && showMenu) // && geom && sample)
		{
			if (imguiBeginScrollArea("Tools", 10, 10, 250, height - 20, &toolsScroll))
				mouseOverMenu = true;

			if (sample)
				sample->handleTools();
			
			imguiEndScrollArea();
		}
		
		// Marker
		if (markerPositionSet && gluProject((GLdouble)markerPosition[0], (GLdouble)markerPosition[1], (GLdouble)markerPosition[2],
								  modelviewMatrix, projectionMatrix, viewport, &x, &y, &z))
		{
			// Draw marker circle
			glLineWidth(5.0f);
			glColor4ub(240,220,0,196);
			glBegin(GL_LINE_LOOP);
			const float r = 25.0f;
			for (int i = 0; i < 20; ++i)
			{
				const float a = (float)i / 20.0f * RC_PI*2;
				const float fx = (float)x + cosf(a)*r;
				const float fy = (float)y + sinf(a)*r;
				glVertex2f(fx,fy);
			}
			glEnd();
			glLineWidth(1.0f);
		}
		
		imguiEndFrame();
		imguiRenderGLDraw();		
		
		glEnable(GL_DEPTH_TEST);
		SDL_GL_SwapWindow(window);
		
	}
	
	imguiRenderGLDestroy();
	
	SDL_Quit();
	
	delete sample;
	delete geom;
	
	return 0;
}
