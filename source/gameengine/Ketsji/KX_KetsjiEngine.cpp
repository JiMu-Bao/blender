/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2001-2002 by NaN Holding BV.
 * All rights reserved.
 * The engine ties all game modules together.
 */

/** \file gameengine/Ketsji/KX_KetsjiEngine.cpp
 *  \ingroup ketsji
 */

#ifdef _MSC_VER
#  pragma warning (disable:4786)
#endif

#include "CM_Message.h"

#include <boost/format.hpp>
#include <thread>

#include "BLI_task.h"

#include "KX_KetsjiEngine.h"

#include "EXP_ListValue.h"
#include "EXP_IntValue.h"
#include "EXP_BoolValue.h"
#include "EXP_FloatValue.h"

#include "RAS_BucketManager.h"
#include "RAS_Rasterizer.h"
#include "RAS_ICanvas.h"
#include "RAS_OffScreen.h"
#include "RAS_Query.h"
#include "RAS_ILightObject.h"
#include "SCA_IInputDevice.h"
#include "KX_Camera.h"
#include "KX_LightObject.h"
#include "KX_Globals.h"
#include "KX_NodeRelationships.h"
#include "KX_PyConstraintBinding.h"
#include "PHY_IPhysicsEnvironment.h"

#include "KX_NetworkMessageScene.h"

#include "DEV_Joystick.h" // for DEV_Joystick::HandleEvents
#include "KX_PythonInit.h" // for updatePythonJoysticks

#include "KX_WorldInfo.h"

#include "BL_Converter.h"
#include "BL_SceneConverter.h"

#include "RAS_FramingManager.h"
#include "DNA_world_types.h"
#include "DNA_scene_types.h"

#include "KX_NavMeshObject.h"

#define DEFAULT_LOGIC_TIC_RATE 60.0

KX_ExitInfo::KX_ExitInfo()
	:m_code(NO_REQUEST)
{
}

const std::string KX_KetsjiEngine::m_profileLabels[tc_numCategories] = {
	"Physics:", // tc_physics
	"Logic:", // tc_logic
	"Animations:", // tc_animations
	"Network:", // tc_network
	"Scenegraph:", // tc_scenegraph
	"Rasterizer:", // tc_rasterizer
	"Services:", // tc_services
	"Overhead:", // tc_overhead
	"Outside:", // tc_outside
	"GPU Latency:" // tc_latency
};

const std::string KX_KetsjiEngine::m_renderQueriesLabels[QUERY_MAX] = {
	"Samples:", // QUERY_SAMPLES
	"Primitives:", // QUERY_PRIMITIVES
	"Time:" // QUERY_TIME
};

/**
 * Constructor of the Ketsji Engine
 */
KX_KetsjiEngine::KX_KetsjiEngine()
	:m_canvas(nullptr),
	m_rasterizer(nullptr),
	m_converter(nullptr),
	m_networkMessageManager(nullptr),
#ifdef WITH_PYTHON
	m_pyprofiledict(PyDict_New()),
#endif
	m_inputDevice(nullptr),
	m_scenes(new EXP_ListValue<KX_Scene>()),
	m_bInitialized(false),
	m_flags(AUTO_ADD_DEBUG_PROPERTIES),
	m_frameTime(0.0f),
	m_clockTime(0.0f),
	m_timescale(1.0f),
	m_previousRealTime(0.0f),
	m_maxLogicFrame(5),
	m_maxPhysicsFrame(5),
	m_ticrate(DEFAULT_LOGIC_TIC_RATE),
	m_anim_framerate(25.0),
	m_doRender(true),
	m_exitKey(SCA_IInputDevice::ENDKEY),
	m_logger(KX_TimeCategoryLogger(m_clock, 25)),
	m_average_framerate(0.0),
	m_showBoundingBox(KX_DebugOption::DISABLE),
	m_showArmature(KX_DebugOption::DISABLE),
	m_showCameraFrustum(KX_DebugOption::DISABLE),
	m_showShadowFrustum(KX_DebugOption::DISABLE),
	m_globalsettings({0}),
	m_taskscheduler(BLI_task_scheduler_create(TASK_SCHEDULER_AUTO_THREADS))
{
	for (int i = tc_first; i < tc_numCategories; i++) {
		m_logger.AddCategory((KX_TimeCategory)i);
	}

	m_renderQueries.emplace_back(RAS_Query::SAMPLES);
	m_renderQueries.emplace_back(RAS_Query::PRIMITIVES);
	m_renderQueries.emplace_back(RAS_Query::TIME);
}

/**
 * Destructor of the Ketsji Engine, release all memory
 */
KX_KetsjiEngine::~KX_KetsjiEngine()
{
#ifdef WITH_PYTHON
	Py_CLEAR(m_pyprofiledict);
#endif

	if (m_taskscheduler) {
		BLI_task_scheduler_free(m_taskscheduler);
	}

	m_scenes->Release();
}

void KX_KetsjiEngine::SetInputDevice(SCA_IInputDevice *inputDevice)
{
	BLI_assert(inputDevice);
	m_inputDevice = inputDevice;
}

void KX_KetsjiEngine::SetCanvas(RAS_ICanvas *canvas)
{
	BLI_assert(canvas);
	m_canvas = canvas;
}

void KX_KetsjiEngine::SetRasterizer(RAS_Rasterizer *rasterizer)
{
	BLI_assert(rasterizer);
	m_rasterizer = rasterizer;
}

void KX_KetsjiEngine::SetNetworkMessageManager(KX_NetworkMessageManager *manager)
{
	m_networkMessageManager = manager;
}

#ifdef WITH_PYTHON
PyObject *KX_KetsjiEngine::GetPyProfileDict()
{
	Py_INCREF(m_pyprofiledict);
	return m_pyprofiledict;
}
#endif

void KX_KetsjiEngine::SetConverter(BL_Converter *converter)
{
	BLI_assert(converter);
	m_converter = converter;
}

void KX_KetsjiEngine::StartEngine()
{
	// Reset the clock to start at 0.0.
	m_clock.Reset();

	m_bInitialized = true;
}

void KX_KetsjiEngine::BeginFrame()
{
	if (m_flags & SHOW_RENDER_QUERIES) {
		m_logger.StartLog(tc_overhead);

		for (RAS_Query& query : m_renderQueries) {
			query.Begin();
		}
	}

	m_logger.StartLog(tc_rasterizer);

	m_rasterizer->BeginFrame(m_frameTime);

	m_canvas->BeginDraw();
}

void KX_KetsjiEngine::EndFrame()
{
	m_rasterizer->MotionBlur();

	m_logger.StartLog(tc_overhead);

	if (m_flags & SHOW_RENDER_QUERIES) {
		for (RAS_Query& query : m_renderQueries) {
			query.End();
		}
	}

	// Show profiling info
	if (m_flags & (SHOW_PROFILE | SHOW_FRAMERATE | SHOW_DEBUG_PROPERTIES | SHOW_RENDER_QUERIES)) {
		RenderDebugProperties();
	}

	double tottime = m_logger.GetAverage();
	if (tottime < 1e-6) {
		tottime = 1e-6;
	}

#ifdef WITH_PYTHON
	for (int i = tc_first; i < tc_numCategories; ++i) {
		double time = m_logger.GetAverage((KX_TimeCategory)i);
		PyObject *val = PyTuple_New(2);
		PyTuple_SetItem(val, 0, PyFloat_FromDouble(time * 1000.0));
		PyTuple_SetItem(val, 1, PyFloat_FromDouble(time / tottime * 100.0));

		PyDict_SetItemString(m_pyprofiledict, m_profileLabels[i].c_str(), val);
		Py_DECREF(val);
	}
#endif

	m_average_framerate = 1.0 / tottime;

	// Go to next profiling measurement, time spent after this call is shown in the next frame.
	m_logger.NextMeasurement();

	m_logger.StartLog(tc_rasterizer);
	m_rasterizer->EndFrame();

	m_logger.StartLog(tc_logic);
	m_canvas->FlushScreenshots(m_rasterizer);

	// swap backbuffer (drawing into this buffer) <-> front/visible buffer
	m_logger.StartLog(tc_latency);
	m_canvas->SwapBuffers();
	m_logger.StartLog(tc_rasterizer);

	m_canvas->EndDraw();
}

KX_KetsjiEngine::FrameTimes KX_KetsjiEngine::GetFrameTimes()
{
	/*
	 * Clock advancement. There is basically two case:
	 *   - USE_EXTERNAL_CLOCK is true, the user is responsible to advance the time
	 *   manually using setClockTime, so here, we do not do anything.
	 *   - USE_EXTERNAL_CLOCK is false, we consider how much
	 *   time has elapsed since last call and we scale this time by the time
	 *   scaling parameter. If m_timescale is 1.0 (default value), the clock
	 *   corresponds to the computer clock.
	 *
	 * Once clockTime has been computed, we will compute how many logic frames
	 * will be executed before the next rendering phase (which will occur at "clockTime").
	 * The game time elapsing between two logic frames (called framestep)
	 * depends on several variables:
	 *   - ticrate
	 *   - max_physic_frame
	 *   - max_logic_frame
	 *   - fixed_framerate
	 */

	// Update time if the user is not controlling it.
	if (!(m_flags & USE_EXTERNAL_CLOCK)) {
		m_clockTime = m_clock.GetTimeSecond();
	}

	// Get elapsed time.
	const double dt = m_clockTime - m_previousRealTime;

	// Time of a frame (without scale).
	double timestep;
	if (m_flags & FIXED_FRAMERATE) {
		// Normal time step for fixed frame.
		timestep = 1.0 / m_ticrate;
	}
	else {
		// The frame is the smallest as possible.
		timestep = dt;
	}

	// Number of frames to proceed.
	int frames;
	if (m_flags & FIXED_FRAMERATE) {
		// As many as possible for the elapsed time.
		frames = int(dt * m_ticrate);
	}
	else {
		// Proceed always one frame in non-fixed framerate.
		frames = 1;
	}

	// Fix timestep to not exceed max physics and logic frames.
	if (frames > m_maxPhysicsFrame) {
		timestep = dt / m_maxPhysicsFrame;
		frames = m_maxPhysicsFrame;
	}
	if (frames > m_maxLogicFrame) {
		timestep = dt / m_maxLogicFrame;
		frames = m_maxLogicFrame;
	}

	// If the number of frame is non-zero, update previous time.
	if (frames > 0) {
		m_previousRealTime = m_clockTime;
	}
	// Else in case of fixed framerate, try to sleep until the next frame.
	else if (m_flags & FIXED_FRAMERATE) {
		const double sleeptime = timestep - dt - 1.0e-3;
		/* If the remaining time is greather than 1ms (sleep resolution) sleep this thread.
		 * The other 1ms will be busy wait.
		 */
		if (sleeptime > 0.0) {
			std::this_thread::sleep_for(std::chrono::nanoseconds((long)(sleeptime * 1.0e9)));
		}
	}

	// Frame time with time scale.
	const double framestep = timestep * m_timescale;

	FrameTimes times;
	times.frames = frames;
	times.timestep = timestep;
	times.framestep = framestep;

	return times;
}

bool KX_KetsjiEngine::NextFrame()
{
	m_logger.StartLog(tc_services);

	const FrameTimes times = GetFrameTimes();

	// Exit if zero frame is sheduled.
	if (times.frames == 0) {
		// Start logging time spent outside main loop
		m_logger.StartLog(tc_outside);

		return false;
	}

	// Fake release events for mouse movements only once.
	m_inputDevice->ReleaseMoveEvent();

	for (unsigned short i = 0; i < times.frames; ++i) {
		m_frameTime += times.framestep;

#ifdef WITH_SDL
		// Handle all SDL Joystick events here to share them for all scenes properly.
		short addrem[JOYINDEX_MAX] = {0};
		if (DEV_Joystick::HandleEvents(addrem)) {
#  ifdef WITH_PYTHON
			updatePythonJoysticks(addrem);
#  endif  // WITH_PYTHON
		}
#endif  // WITH_SDL

		// for each scene, call the proceed functions
		for (KX_Scene *scene : m_scenes) {
			/* Suspension holds the physics and logic processing for an
			 * entire scene. Objects can be suspended individually, and
			 * the settings for that precede the logic and physics
			 * update. */
			m_logger.StartLog(tc_logic);

			scene->UpdateObjectActivity();

			if (!scene->IsSuspended()) {
				m_logger.StartLog(tc_physics);
				// set Python hooks for each scene
				KX_SetActiveScene(scene);

				// Process sensors, and controllers
				m_logger.StartLog(tc_logic);
				scene->LogicBeginFrame(m_frameTime, times.framestep);

				// Scenegraph needs to be updated again, because Logic Controllers
				// can affect the local matrices.
				m_logger.StartLog(tc_scenegraph);
				scene->UpdateParents();

				// Process actuators

				// Do some cleanup work for this logic frame
				m_logger.StartLog(tc_logic);
				scene->LogicUpdateFrame(m_frameTime);

				scene->LogicEndFrame();

				// Actuators can affect the scenegraph
				m_logger.StartLog(tc_scenegraph);
				scene->UpdateParents();

				m_logger.StartLog(tc_physics);

				// Perform physics calculations on the scene. This can involve
				// many iterations of the physics solver.
				scene->GetPhysicsEnvironment()->ProceedDeltaTime(m_frameTime, times.timestep, times.framestep);//m_deltatimerealDeltaTime);

				m_logger.StartLog(tc_scenegraph);
				scene->UpdateParents();
			}

			m_logger.StartLog(tc_services);
		}

		m_logger.StartLog(tc_network);
		m_networkMessageManager->ClearMessages();

		// update system devices
		m_logger.StartLog(tc_logic);
		m_inputDevice->ClearInputs();

		m_converter->ProcessScheduledLibraries();

		UpdateSuspendedScenes(times.framestep);
		// scene management
		ProcessScheduledScenes();
	}

	// Start logging time spent outside main loop
	m_logger.StartLog(tc_outside);

	return m_doRender;
}

void KX_KetsjiEngine::UpdateSuspendedScenes(double framestep)
{
	for (KX_Scene *scene : m_scenes) {
		if (scene->IsSuspended()) {
			scene->SetSuspendedDelta(scene->GetSuspendedDelta() + framestep);
		}
	}
}

KX_CameraRenderSchedule KX_KetsjiEngine::ScheduleCameraRender(KX_Scene *scene, KX_Camera *camera, KX_Camera *overrideCullingCam,
		const RAS_Rect& displayArea, RAS_Rasterizer::StereoMode stereoMode, RAS_Rasterizer::StereoEye eye,
		unsigned short viewportIndex)
{
	KX_SetActiveScene(scene);
#ifdef WITH_PYTHON
	scene->RunDrawingCallbacks(KX_Scene::PRE_DRAW_SETUP, camera);
#endif

	KX_CameraRenderSchedule cameraSchedule;

	RAS_Rect area;
	RAS_Rect viewport;
	// Compute the area and the viewport based on the current display area and the optional camera viewport.
	GetSceneViewport(scene, camera, displayArea, area, viewport);
	cameraSchedule.m_area = area;
	cameraSchedule.m_viewport = viewport;

	// Compute the camera matrices: modelview and projection.
	camera->UpdateView(m_rasterizer, scene, stereoMode, eye, viewport, area);

	cameraSchedule.m_viewMatrix = camera->GetModelviewMatrix(eye);
	cameraSchedule.m_progMatrix = camera->GetProjectionMatrix(eye);
	cameraSchedule.m_camTrans = camera->GetWorldToCamera();
	cameraSchedule.m_negScale = camera->IsNegativeScaling();
	cameraSchedule.m_perspective = camera->GetCameraData()->m_perspective;
	cameraSchedule.m_frameFrustum = camera->GetFrameFrustum();
	cameraSchedule.m_camera = camera;

	KX_Camera *cullingcam;
	if (overrideCullingCam) {
		cullingcam = overrideCullingCam;
		// Compute the area and the viewport based on the current display area and the optional camera viewport.
		GetSceneViewport(scene, overrideCullingCam, displayArea, area, viewport);
		// Compute the camera matrices: modelview and projection.
		overrideCullingCam->UpdateView(m_rasterizer, scene, stereoMode, eye, viewport, area);
	}
	else {
		cullingcam = camera;
	}

	cameraSchedule.m_position = cullingcam->NodeGetWorldPosition();
	cameraSchedule.m_frustum = cullingcam->GetFrustum(eye);
	cameraSchedule.m_culling = cullingcam->GetFrustumCulling();
	cameraSchedule.m_lodFactor = cullingcam->GetLodDistanceFactor();
	cameraSchedule.m_stereoMode = stereoMode;
	cameraSchedule.m_eye = eye;
	cameraSchedule.m_focalLength = camera->GetFocalLength();
	cameraSchedule.m_index = viewportIndex;

	return cameraSchedule;
}

KX_RenderSchedule KX_KetsjiEngine::ScheduleRender()
{
	const RAS_Rasterizer::StereoMode stereoMode = m_rasterizer->GetStereoMode();
	const bool useStereo = (stereoMode != RAS_Rasterizer::RAS_STEREO_NOSTEREO);
	// Set to true when each eye needs to be rendered in a separated off screen.
	const bool renderPerEye = ELEM(stereoMode, RAS_Rasterizer::RAS_STEREO_INTERLACED, 
			RAS_Rasterizer::RAS_STEREO_VINTERLACE, RAS_Rasterizer::RAS_STEREO_ANAGLYPH);
	// The number of eyes to manage in case of stereo.
	const unsigned short numeyes = (useStereo) ? 2 : 1;
	// The number of frames in case of stereo, could be multiple for interlaced or anaglyph stereo.
	const unsigned short numframes = (renderPerEye) ? 2 : 1;

	// The off screen corresponding to the frame.
	static const RAS_OffScreen::Type ofsType[] = {
		RAS_OffScreen::RAS_OFFSCREEN_EYE_LEFT0,
		RAS_OffScreen::RAS_OFFSCREEN_EYE_RIGHT0
	};

	KX_RenderSchedule renderSchedule;
	renderSchedule.m_renderPerEye = renderPerEye;

	for (unsigned short index = 0; index < numframes; ++index) {
		KX_FrameRenderSchedule frameSchedule;
		frameSchedule.m_ofsType = ofsType[index];

		// Get the eyes managed per frame.
		std::vector<RAS_Rasterizer::StereoEye> eyes;
		// Only one eye for unique frame.
		if (!useStereo) {
			frameSchedule.m_eyes = {RAS_Rasterizer::RAS_STEREO_LEFTEYE};
		}
		// One eye per frame but different.
		else if (renderPerEye) {
			frameSchedule.m_eyes = {(RAS_Rasterizer::StereoEye)index};
		}
		// Two eyes for unique frame.
		else {
			frameSchedule.m_eyes = {RAS_Rasterizer::RAS_STEREO_LEFTEYE, RAS_Rasterizer::RAS_STEREO_RIGHTEYE};
		}

		renderSchedule.m_frameSchedules.push_back(frameSchedule);
	}

	// Pre-compute the display area used for stereo or normal rendering.
	RAS_Rect displayAreas[RAS_Rasterizer::RAS_STEREO_MAXEYE];
	for (unsigned short eye = 0; eye < numeyes; ++eye) {
		displayAreas[eye] = m_rasterizer->GetRenderArea(m_canvas, stereoMode, (RAS_Rasterizer::StereoEye)eye);
	}

	const bool doTextures = (m_rasterizer->GetDrawingMode() == RAS_Rasterizer::RAS_TEXTURED);

	for (KX_Scene *scene : m_scenes) {
		KX_SceneRenderSchedule sceneSchedule;
		sceneSchedule.m_scene = scene;

		KX_Camera *activecam = scene->GetActiveCamera();
		KX_Camera *overrideCullingCam = scene->GetOverrideCullingCamera();
		unsigned int viewportIndex = 0;
		for (KX_Camera *cam : scene->GetCameraList()) {
			if (cam != activecam && !cam->UseViewport()) {
				continue;
			}

			for (unsigned short eye = 0; eye < numeyes; ++eye) {
				sceneSchedule.m_cameraSchedules[eye].push_back(ScheduleCameraRender(scene, cam, overrideCullingCam,
							displayAreas[eye], stereoMode, (RAS_Rasterizer::StereoEye)eye, viewportIndex++));
			}
		}

		// Schedule texture rendering for shadows and cube/planar map.
		if (doTextures) {
			scene->UpdateLights(m_rasterizer);

			// Get the shadow schedules.
			const KX_TextureRenderScheduleList shadowSchedule = scene->ScheduleShadowsRender();
			// Get the renderer schedules.
			const KX_TextureRenderScheduleList textureSchedule = scene->ScheduleTexturesRender(m_rasterizer, sceneSchedule);

			// Merge both in texture schedules.
			sceneSchedule.m_textureSchedules.insert(sceneSchedule.m_textureSchedules.end(), shadowSchedule.begin(), shadowSchedule.end());
			sceneSchedule.m_textureSchedules.insert(sceneSchedule.m_textureSchedules.end(), textureSchedule.begin(), textureSchedule.end());
		}

		renderSchedule.m_sceneSchedules.push_back(sceneSchedule);
	}

	return renderSchedule;
}

void KX_KetsjiEngine::Render()
{
	m_logger.StartLog(tc_rasterizer);

	BeginFrame();

	// Get whole rendering schedule.
	KX_RenderSchedule renderSchedule = ScheduleRender();

	// Render textures (shadows and renderers).
	for (const KX_SceneRenderSchedule& sceneSchedule : renderSchedule.m_sceneSchedules) {
		for (const KX_TextureRenderSchedule& textureSchedule : sceneSchedule.m_textureSchedules) {
			RenderTexture(sceneSchedule.m_scene, textureSchedule);
		}
	}

	const int width = m_canvas->GetWidth();
	const int height = m_canvas->GetHeight();
	// clear the entire game screen with the border color
	// only once per frame
	m_rasterizer->SetViewport(0, 0, width, height);
	m_rasterizer->SetScissor(0, 0, width, height);

	const RAS_FrameSettings &framesettings = renderSchedule.m_frameSettings;
	// Use the framing bar color set in the Blender scenes
	m_rasterizer->SetClearColor(framesettings.BarRed(), framesettings.BarGreen(), framesettings.BarBlue(), 1.0f);

	// Used to detect when a camera is the first rendered an then doesn't request a depth clear.
	unsigned short pass = 0;

	for (KX_FrameRenderSchedule& frameSchedule : renderSchedule.m_frameSchedules) {
		// Current bound off screen.
		RAS_OffScreen *offScreen = m_canvas->GetOffScreen(frameSchedule.m_ofsType);
		offScreen->Bind();

		// Clear off screen only before the first scene render.
		m_rasterizer->Clear(RAS_Rasterizer::RAS_COLOR_BUFFER_BIT | RAS_Rasterizer::RAS_DEPTH_BUFFER_BIT);

		// for each scene, call the proceed functions
		for (unsigned short i = 0, size = renderSchedule.m_sceneSchedules.size(); i < size; ++i) {
			const KX_SceneRenderSchedule& sceneSchedule = renderSchedule.m_sceneSchedules[i];
			KX_Scene *scene = sceneSchedule.m_scene;

			const bool isfirstscene = (i == 0);
			const bool islastscene = (i == (size - 1));

			// pass the scene's worldsettings to the rasterizer
			scene->GetWorldInfo()->UpdateWorldSettings(m_rasterizer);

			m_rasterizer->SetAuxilaryClientInfo(scene);

			// Render the eyes handled by the frame.
			for (RAS_Rasterizer::StereoEye eye : frameSchedule.m_eyes) {
				// Draw the scene once for each camera with an enabled viewport or an active camera.
				for (const KX_CameraRenderSchedule& cameraSchedule : sceneSchedule.m_cameraSchedules[eye]) {
					// do the rendering
					RenderCamera(scene, cameraSchedule, offScreen, pass++, isfirstscene);
				}
			}

			// Render filters and get output off screen.
			offScreen = PostRenderScene(scene, offScreen, frameSchedule, islastscene);
			frameSchedule.m_ofsType = offScreen->GetType();
		}
	}

	m_canvas->SetViewPort(0, 0, width, height);

	// Compositing per eye off screens to screen.
	if (renderSchedule.m_renderPerEye) {
		RAS_OffScreen *leftofs = m_canvas->GetOffScreen(renderSchedule.m_frameSchedules[0].m_ofsType);
		RAS_OffScreen *rightofs = m_canvas->GetOffScreen(renderSchedule.m_frameSchedules[1].m_ofsType);
		m_rasterizer->DrawStereoOffScreenToScreen(m_canvas, leftofs, rightofs, renderSchedule.m_stereoMode);
	}
	// Else simply draw the off screen to screen.
	else {
		m_rasterizer->DrawOffScreenToScreen(m_canvas, m_canvas->GetOffScreen(renderSchedule.m_frameSchedules[0].m_ofsType));
	}

	EndFrame();
}

void KX_KetsjiEngine::RequestExit(KX_ExitInfo::Code code)
{
	RequestExit(code, "");
}

void KX_KetsjiEngine::RequestExit(KX_ExitInfo::Code code, const std::string& fileName)
{
	m_exitInfo.m_code = code;
	m_exitInfo.m_fileName = fileName;
}

const KX_ExitInfo& KX_KetsjiEngine::GetExitInfo() const
{
	return m_exitInfo;
}

void KX_KetsjiEngine::EnableCameraOverride(const std::string& forscene, const mt::mat3& orientation,
		const mt::vec3& position, const RAS_CameraData& camdata)
{
	SetFlag(CAMERA_OVERRIDE, true);

	m_overrideSceneName = forscene;
	m_overrideCamOrientation = orientation;
	m_overrideCamPosition = position;
	m_overrideCamData = camdata;
}


void KX_KetsjiEngine::GetSceneViewport(KX_Scene *scene, KX_Camera *cam, const RAS_Rect& displayArea, RAS_Rect& area, RAS_Rect& viewport)
{
	// In this function we make sure the rasterizer settings are up-to-date.
	// We compute the viewport so that logic using this information is up-to-date.

	// Note we postpone computation of the projection matrix
	// so that we are using the latest camera position.

	if (cam->UseViewport()) {
		area = cam->GetViewport();
	}
	else {
		area = displayArea;
	}

	RAS_FramingManager::ComputeViewport(scene->GetFramingType(), area, viewport);
}

void KX_KetsjiEngine::UpdateAnimations(KX_Scene *scene)
{
	if (scene->IsSuspended()) {
		return;
	}

	scene->UpdateAnimations(m_frameTime, (m_flags & RESTRICT_ANIMATION) != 0);
}

void KX_KetsjiEngine::RenderTexture(KX_Scene *scene, const KX_TextureRenderSchedule& textureSchedule)
{
	m_logger.StartLog(tc_scenegraph);

	// Obtain visible renderable objects.
	const std::vector<KX_GameObject *> objects = scene->CalculateVisibleMeshes(textureSchedule.m_frustum, textureSchedule.m_visibleLayers);

	// Update levels of detail.
	if (textureSchedule.m_mode & KX_TextureRenderSchedule::MODE_UPDATE_LOD) {
		scene->UpdateObjectLods(textureSchedule.m_position, textureSchedule.m_lodFactor, objects);
	}

	m_logger.StartLog(tc_animations);

	UpdateAnimations(scene);

	m_logger.StartLog(tc_rasterizer);

	// Texture render are not depending on viewport.
	m_rasterizer->Disable(RAS_Rasterizer::RAS_SCISSOR_TEST);

	m_rasterizer->SetEye(textureSchedule.m_eye);
	m_rasterizer->SetProjectionMatrix(textureSchedule.m_progMatrix);
	m_rasterizer->SetViewMatrix(textureSchedule.m_viewMatrix);

	// Bind texture target and update settings.
	textureSchedule.m_bind(m_rasterizer);
	m_rasterizer->Clear(textureSchedule.m_clearMode);

	// Optionally render world background.
	if (textureSchedule.m_mode & KX_TextureRenderSchedule::MODE_RENDER_WORLD) {
		KX_WorldInfo *worldInfo = scene->GetWorldInfo();
		// Update background and render it.
		worldInfo->UpdateBackGround(m_rasterizer);
		worldInfo->RenderBackground(m_rasterizer);
	}

	// Render the scene.
	scene->RenderBuckets(objects, textureSchedule.m_drawingMode, textureSchedule.m_camTrans, textureSchedule.m_index, m_rasterizer, nullptr);

	// Unbind texture target.
	textureSchedule.m_unbind(m_rasterizer);

	m_rasterizer->Enable(RAS_Rasterizer::RAS_SCISSOR_TEST);
}

void KX_KetsjiEngine::RenderCamera(KX_Scene *scene, const KX_CameraRenderSchedule& cameraSchedule, RAS_OffScreen *offScreen,
                                   unsigned short pass, bool isFirstScene)
{

	KX_SetActiveScene(scene);

	m_logger.StartLog(tc_scenegraph);

	const std::vector<KX_GameObject *> objects = scene->CalculateVisibleMeshes(cameraSchedule.m_culling, cameraSchedule.m_frustum, 0);

	// update levels of detail
	scene->UpdateObjectLods(cameraSchedule.m_position, cameraSchedule.m_lodFactor, objects);

	m_logger.StartLog(tc_animations);

	UpdateAnimations(scene);

	m_logger.StartLog(tc_rasterizer);

	const RAS_Rect &viewport = cameraSchedule.m_viewport;
	// set the viewport for this frame and scene
	m_rasterizer->SetViewport(viewport);
	m_rasterizer->SetScissor(viewport);

	/* Clear the depth after setting the scene viewport/scissor
	 * if it's not the first render pass. */
	if (pass > 0) {
		m_rasterizer->Clear(RAS_Rasterizer::RAS_DEPTH_BUFFER_BIT);
	}

	m_rasterizer->SetEye(cameraSchedule.m_eye);
	m_rasterizer->SetProjectionMatrix(cameraSchedule.m_progMatrix);
	m_rasterizer->SetViewMatrix(cameraSchedule.m_viewMatrix, cameraSchedule.m_negScale);

	if (isFirstScene) {
		KX_WorldInfo *worldInfo = scene->GetWorldInfo();
		// Update background and render it.
		worldInfo->UpdateBackGround(m_rasterizer);
		worldInfo->RenderBackground(m_rasterizer);
	}

	// Draw debug infos like bouding box, armature ect.. if enabled.
	scene->DrawDebug(objects, m_showBoundingBox, m_showArmature);
	// Draw debug camera frustum.
	DrawDebugCameraFrustum(scene, cameraSchedule);
	DrawDebugShadowFrustum(scene);

#ifdef WITH_PYTHON
	// Run any pre-drawing python callbacks
	scene->RunDrawingCallbacks(KX_Scene::PRE_DRAW, cameraSchedule.m_camera);
#endif

	scene->RenderBuckets(objects, m_rasterizer->GetDrawingMode(), cameraSchedule.m_camTrans, cameraSchedule.m_index, m_rasterizer, offScreen);

	if (scene->GetPhysicsEnvironment()) {
		scene->GetPhysicsEnvironment()->DebugDrawWorld();
	}
}

/*
 * To run once per scene
 */
RAS_OffScreen *KX_KetsjiEngine::PostRenderScene(KX_Scene *scene, RAS_OffScreen *inputofs,
		const KX_FrameRenderSchedule& frameSchedule, bool islastscene)
{
	KX_SetActiveScene(scene);

	/* Choose final render off screen target. If the current off screen is using multisamples we
	 * are sure that it will be copied to a non-multisamples off screen before render the filters.
	 * In this case the targeted off screen is the same as the current off screen. */
	RAS_OffScreen::Type target;
	if (inputofs->GetSamples() > 0) {
		/* If the last scene is rendered it's useless to specify a multisamples off screen, we use then
		 * a non-multisamples off screen and avoid an extra off screen blit. */
		if (islastscene) {
			target = RAS_OffScreen::NextRenderOffScreen(frameSchedule.m_ofsType);
		}
		else {
			target = frameSchedule.m_ofsType;
		}
	}
	/* In case of non-multisamples a ping pong per scene render is made between a potentially multisamples
	 * off screen and a non-multisamples off screen as the both doesn't use multisamples. */
	else {
		target = RAS_OffScreen::NextRenderOffScreen(frameSchedule.m_ofsType);
	}

	RAS_OffScreen *targetofs = m_canvas->GetOffScreen(target);

	scene->FlushDebugDraw(m_rasterizer, m_canvas);

	// We need to first make sure our viewport is correct (enabling multiple viewports can mess this up), only for filters.
	const int width = m_canvas->GetWidth();
	const int height = m_canvas->GetHeight();
	m_rasterizer->SetViewport(0, 0, width, height);
	m_rasterizer->SetScissor(0, 0, width, height);

	RAS_OffScreen *offScreen = scene->Render2DFilters(m_rasterizer, m_canvas, inputofs, targetofs);

#ifdef WITH_PYTHON
	/* We can't deduce what camera should be passed to the python callbacks
	 * because the post draw callbacks are per scenes and not per cameras.
	 */
	scene->RunDrawingCallbacks(KX_Scene::POST_DRAW, nullptr);

	// Python draw callback can also call debug draw functions, so we have to clear debug shapes.
	scene->FlushDebugDraw(m_rasterizer, m_canvas);
#endif

	return offScreen;
}

void KX_KetsjiEngine::StopEngine()
{
	if (m_bInitialized) {
		m_converter->FinalizeAsyncLoads();

		while (m_scenes->GetCount() > 0) {
			KX_Scene *scene = m_scenes->GetFront();
			DestructScene(scene);
			// WARNING: here the scene is a dangling pointer.
			m_scenes->Remove(0);
		}

		// cleanup all the stuff
		m_rasterizer->Exit();
	}
}

// Scene Management is able to switch between scenes
// and have several scenes running in parallel
void KX_KetsjiEngine::AddScene(KX_Scene *scene)
{
	m_scenes->Add(CM_AddRef(scene));
	PostProcessScene(scene);
}

void KX_KetsjiEngine::PostProcessScene(KX_Scene *scene)
{
	bool override_camera = (((m_flags & CAMERA_OVERRIDE) != 0) && (scene->GetName() == m_overrideSceneName));

	// if there is no activecamera, or the camera is being
	// overridden we need to construct a temporary camera
	if (!scene->GetActiveCamera() || override_camera) {
		KX_Camera *activecam = nullptr;

		activecam = new KX_Camera(override_camera ? m_overrideCamData : RAS_CameraData());
		activecam->SetNode(new SG_Node(activecam, scene, KX_Scene::m_callbacks, (new KX_NormalParentRelation())));
		activecam->SetName("__default__cam__");

		// set transformation
		if (override_camera) {
			activecam->NodeSetLocalPosition(m_overrideCamPosition);
			activecam->NodeSetLocalOrientation(m_overrideCamOrientation);
		}
		else {
			activecam->NodeSetLocalPosition(mt::zero3);
			activecam->NodeSetLocalOrientation(mt::mat3::Identity());
		}

		activecam->NodeUpdate();

		scene->GetCameraList()->Add(CM_AddRef(activecam));
		scene->SetActiveCamera(activecam);
		scene->GetObjectList()->Add(CM_AddRef(activecam));
		// done with activecam
		activecam->Release();
	}

	scene->UpdateParents();
}

void KX_KetsjiEngine::RenderDebugProperties()
{
	std::string debugtxt;
	int title_xmargin = -7;
	int title_y_top_margin = 4;
	int title_y_bottom_margin = 2;

	int const_xindent = 4;
	int const_ysize = 14;

	int xcoord = 12;    // mmmm, these constants were taken from blender source
	int ycoord = 17;    // to 'mimic' behavior

	int profile_indent = 72;

	float tottime = m_logger.GetAverage();
	if (tottime < 1e-6f) {
		tottime = 1e-6f;
	}

	static const mt::vec4 white(1.0f, 1.0f, 1.0f, 1.0f);

	if (m_flags & (SHOW_FRAMERATE | SHOW_PROFILE)) {
		// Title for profiling("Profile")
		// Adds the constant x indent (0 for now) to the title x margin
		m_debugDraw.RenderText2d("Profile", mt::vec2(xcoord + const_xindent + title_xmargin, ycoord), white);

		// Increase the indent by default increase
		ycoord += const_ysize;
		// Add the title indent afterwards
		ycoord += title_y_bottom_margin;
	}

	// Framerate display
	if (m_flags & SHOW_FRAMERATE) {
		m_debugDraw.RenderText2d("Frametime :",
		                       mt::vec2(xcoord + const_xindent,
		                                ycoord), white);

		debugtxt = (boost::format("%5.2fms (%.1ffps)") %  (tottime * 1000.0f) % (1.0f / tottime)).str();
		m_debugDraw.RenderText2d(debugtxt, mt::vec2(xcoord + const_xindent + profile_indent, ycoord), white);
		// Increase the indent by default increase
		ycoord += const_ysize;
	}

	// Profile display
	if (m_flags & SHOW_PROFILE) {
		for (int j = tc_first; j < tc_numCategories; j++) {
			m_debugDraw.RenderText2d(m_profileLabels[j], mt::vec2(xcoord + const_xindent, ycoord), white);

			double time = m_logger.GetAverage((KX_TimeCategory)j);

			debugtxt = (boost::format("%5.2fms | %d%%") % (time * 1000.f) % (int)(time / tottime * 100.f)).str();
			m_debugDraw.RenderText2d(debugtxt, mt::vec2(xcoord + const_xindent + profile_indent, ycoord), white);

			const mt::vec2 boxSize(50 * (time / tottime), 9);
			m_debugDraw.RenderBox2d(mt::vec2(xcoord + (int)(2.2 * profile_indent), ycoord), boxSize, white);
			ycoord += const_ysize;
		}
	}

	if (m_flags & SHOW_RENDER_QUERIES) {
		m_debugDraw.RenderText2d("Render Queries :", mt::vec2(xcoord + const_xindent + title_xmargin, ycoord), white);
		ycoord += const_ysize;

		for (unsigned short i = 0; i < QUERY_MAX; ++i) {
			m_debugDraw.RenderText2d(m_renderQueriesLabels[i], mt::vec2(xcoord + const_xindent, ycoord), white);

			if (i == QUERY_TIME) {
				debugtxt = (boost::format("%.2fms") % (((float)m_renderQueries[i].Result()) / 1e6)).str();
			}
			else {
				debugtxt = (boost::format("%i") % m_renderQueries[i].Result()).str();
			}

			m_debugDraw.RenderText2d(debugtxt, mt::vec2(xcoord + const_xindent + profile_indent, ycoord), white);
			ycoord += const_ysize;
		}
	}

	// Add the ymargin for titles below the other section of debug info
	ycoord += title_y_top_margin;

	/* Property display */
	if (m_flags & SHOW_DEBUG_PROPERTIES) {
		// Title for debugging("Debug properties")
		// Adds the constant x indent (0 for now) to the title x margin
		m_debugDraw.RenderText2d("Debug Properties", mt::vec2(xcoord + const_xindent + title_xmargin, ycoord), white);

		// Increase the indent by default increase
		ycoord += const_ysize;
		// Add the title indent afterwards
		ycoord += title_y_bottom_margin;

		/* Calculate amount of properties that can displayed. */
		const unsigned short propsMax = (m_canvas->GetHeight() - ycoord) / const_ysize;

		for (KX_Scene *scene : m_scenes) {
			scene->RenderDebugProperties(m_debugDraw, const_xindent, const_ysize, xcoord, ycoord, propsMax);
		}
	}

	m_debugDraw.Flush(m_rasterizer, m_canvas);
}

void KX_KetsjiEngine::DrawDebugCameraFrustum(KX_Scene *scene, const KX_CameraRenderSchedule& cameraSchedule)
{
	if (m_showCameraFrustum == KX_DebugOption::DISABLE) {
		return;
	}

	RAS_DebugDraw& debugDraw = scene->GetDebugDraw();
	for (KX_Camera *cam : scene->GetCameraList()) {
		if (cam != cameraSchedule.m_camera && (m_showCameraFrustum == KX_DebugOption::FORCE || cam->GetShowCameraFrustum())) {
			cam->UpdateView(m_rasterizer, scene, cameraSchedule.m_stereoMode, cameraSchedule.m_eye,
					cameraSchedule.m_viewport, cameraSchedule.m_area);

			debugDraw.DrawCameraFrustum(
				cam->GetProjectionMatrix(cameraSchedule.m_eye) * cam->GetModelviewMatrix(cameraSchedule.m_eye));
		}
	}
}

void KX_KetsjiEngine::DrawDebugShadowFrustum(KX_Scene *scene)
{
	if (m_showShadowFrustum == KX_DebugOption::DISABLE) {
		return;
	}

	RAS_DebugDraw& debugDraw = scene->GetDebugDraw();
	for (KX_LightObject *light : scene->GetLightList()) {
		RAS_ILightObject *raslight = light->GetLightData();
		if (m_showShadowFrustum == KX_DebugOption::FORCE || light->GetShowShadowFrustum()) {
			const mt::mat4 projmat(raslight->GetWinMat());
			const mt::mat4 viewmat(raslight->GetViewMat());

			debugDraw.DrawCameraFrustum(projmat * viewmat);
		}
	}
}

EXP_ListValue<KX_Scene> *KX_KetsjiEngine::CurrentScenes()
{
	return m_scenes;
}

KX_Scene *KX_KetsjiEngine::FindScene(const std::string& scenename)
{
	return m_scenes->FindValue(scenename);
}

void KX_KetsjiEngine::ConvertAndAddScene(const std::string& scenename, bool overlay)
{
	// only add scene when it doesn't exist!
	if (FindScene(scenename)) {
		CM_Warning("scene " << scenename << " already exists, not added!");
	}
	else {
		if (overlay) {
			m_addingOverlayScenes.push_back(scenename);
		}
		else {
			m_addingBackgroundScenes.push_back(scenename);
		}
	}
}

void KX_KetsjiEngine::RemoveScene(const std::string& scenename)
{
	if (FindScene(scenename)) {
		m_removingScenes.push_back(scenename);
	}
	else {
		CM_Warning("scene " << scenename << " does not exist, not removed!");
	}
}

void KX_KetsjiEngine::RemoveScheduledScenes()
{
	if (!m_removingScenes.empty()) {
		std::vector<std::string>::iterator scenenameit;
		for (scenenameit = m_removingScenes.begin(); scenenameit != m_removingScenes.end(); scenenameit++) {
			std::string scenename = *scenenameit;

			KX_Scene *scene = FindScene(scenename);
			if (scene) {
				DestructScene(scene);
				m_scenes->RemoveValue(scene);
			}
		}
		m_removingScenes.clear();
	}
}

KX_Scene *KX_KetsjiEngine::CreateScene(Scene *scene)
{
	KX_Scene *tmpscene = new KX_Scene(m_inputDevice,
	                                  scene->id.name + 2,
	                                  scene,
	                                  m_networkMessageManager);

	return tmpscene;
}

KX_Scene *KX_KetsjiEngine::CreateScene(const std::string& scenename)
{
	Scene *scene = m_converter->GetBlenderSceneForName(scenename);
	if (!scene) {
		return nullptr;
	}

	return CreateScene(scene);
}

void KX_KetsjiEngine::AddScheduledScenes()
{
	if (!m_addingOverlayScenes.empty()) {
		for (const std::string& scenename : m_addingOverlayScenes) {
			KX_Scene *tmpscene = CreateScene(scenename);

			if (tmpscene) {
				m_converter->ConvertScene(tmpscene);
				m_scenes->Add(CM_AddRef(tmpscene));
				PostProcessScene(tmpscene);
				tmpscene->Release();
			}
			else {
				CM_Warning("scene " << scenename << " could not be found, not added!");
			}
		}
		m_addingOverlayScenes.clear();
	}

	if (!m_addingBackgroundScenes.empty()) {
		for (const std::string& scenename : m_addingBackgroundScenes) {
			KX_Scene *tmpscene = CreateScene(scenename);

			if (tmpscene) {
				m_converter->ConvertScene(tmpscene);
				m_scenes->Insert(0, CM_AddRef(tmpscene));
				PostProcessScene(tmpscene);
				tmpscene->Release();
			}
			else {
				CM_Warning("scene " << scenename << " could not be found, not added!");
			}
		}
		m_addingBackgroundScenes.clear();
	}
}

bool KX_KetsjiEngine::ReplaceScene(const std::string& oldscene, const std::string& newscene)
{
	// Don't allow replacement if the new scene doesn't exist.
	// Allows smarter game design (used to have no check here).
	// Note that it creates a small backward compatbility issue
	// for a game that did a replace followed by a lib load with the
	// new scene in the lib => it won't work anymore, the lib
	// must be loaded before doing the replace.
	if (m_converter->GetBlenderSceneForName(newscene)) {
		m_replace_scenes.emplace_back(oldscene, newscene);
		return true;
	}

	return false;
}

// replace scene is not the same as removing and adding because the
// scene must be in exact the same place (to maintain drawingorder)
// (nzc) - should that not be done with a scene-display list? It seems
// stupid to rely on the mem allocation order...
void KX_KetsjiEngine::ReplaceScheduledScenes()
{
	if (!m_replace_scenes.empty()) {
		std::vector<std::pair<std::string, std::string> >::iterator scenenameit;

		for (scenenameit = m_replace_scenes.begin();
		     scenenameit != m_replace_scenes.end();
		     scenenameit++)
		{
			std::string oldscenename = (*scenenameit).first;
			std::string newscenename = (*scenenameit).second;
			/* Scenes are not supposed to be included twice... I think */
			for (unsigned int sce_idx = 0; sce_idx < m_scenes->GetCount(); ++sce_idx) {
				KX_Scene *scene = m_scenes->GetValue(sce_idx);
				if (scene->GetName() == oldscenename) {
					// avoid crash if the new scene doesn't exist, just do nothing
					Scene *blScene = m_converter->GetBlenderSceneForName(newscenename);
					if (blScene) {
						DestructScene(scene);

						KX_Scene *tmpscene = CreateScene(blScene);
						m_converter->ConvertScene(tmpscene);

						m_scenes->SetValue(sce_idx, CM_AddRef(tmpscene));
						PostProcessScene(tmpscene);
						tmpscene->Release();
					}
					else {
						CM_Warning("scene " << newscenename << " could not be found, not replaced!");
					}
				}
			}
		}
		m_replace_scenes.clear();
	}
}

void KX_KetsjiEngine::SuspendScene(const std::string& scenename)
{
	KX_Scene *scene = FindScene(scenename);
	if (scene) {
		scene->Suspend();
	}
}

void KX_KetsjiEngine::ResumeScene(const std::string& scenename)
{
	KX_Scene *scene = FindScene(scenename);
	if (scene) {
		scene->Resume();
	}
}

void KX_KetsjiEngine::DestructScene(KX_Scene *scene)
{
	scene->RunOnRemoveCallbacks();
	m_converter->RemoveScene(scene);
}

double KX_KetsjiEngine::GetTicRate()
{
	return m_ticrate;
}

void KX_KetsjiEngine::SetTicRate(double ticrate)
{
	m_ticrate = ticrate;
}

double KX_KetsjiEngine::GetTimeScale() const
{
	return m_timescale;
}

void KX_KetsjiEngine::SetTimeScale(double timeScale)
{
	m_timescale = timeScale;
}

int KX_KetsjiEngine::GetMaxLogicFrame()
{
	return m_maxLogicFrame;
}

void KX_KetsjiEngine::SetMaxLogicFrame(int frame)
{
	m_maxLogicFrame = frame;
}

int KX_KetsjiEngine::GetMaxPhysicsFrame()
{
	return m_maxPhysicsFrame;
}

void KX_KetsjiEngine::SetMaxPhysicsFrame(int frame)
{
	m_maxPhysicsFrame = frame;
}

double KX_KetsjiEngine::GetAnimFrameRate()
{
	return m_anim_framerate;
}

bool KX_KetsjiEngine::GetFlag(FlagType flag) const
{
	return (m_flags & flag) != 0;
}

void KX_KetsjiEngine::SetFlag(FlagType flag, bool enable)
{
	if (enable) {
		m_flags = (FlagType)(m_flags | flag);
	}
	else {
		m_flags = (FlagType)(m_flags & ~flag);
	}
}

double KX_KetsjiEngine::GetClockTime() const
{
	return m_clockTime;
}

void KX_KetsjiEngine::SetClockTime(double externalClockTime)
{
	m_clockTime = externalClockTime;
}

double KX_KetsjiEngine::GetFrameTime() const
{
	return m_frameTime;
}

double KX_KetsjiEngine::GetRealTime() const
{
	return m_clock.GetTimeSecond();
}

void KX_KetsjiEngine::SetAnimFrameRate(double framerate)
{
	m_anim_framerate = framerate;
}

double KX_KetsjiEngine::GetAverageFrameRate()
{
	return m_average_framerate;
}

void KX_KetsjiEngine::SetExitKey(SCA_IInputDevice::SCA_EnumInputs key)
{
	m_exitKey = key;
}

SCA_IInputDevice::SCA_EnumInputs KX_KetsjiEngine::GetExitKey() const
{
	return m_exitKey;
}

void KX_KetsjiEngine::SetRender(bool render)
{
	m_doRender = render;
}

bool KX_KetsjiEngine::GetRender()
{
	return m_doRender;
}

void KX_KetsjiEngine::ProcessScheduledScenes()
{
	// Check whether there will be changes to the list of scenes
	if (!(m_addingOverlayScenes.empty() && m_addingBackgroundScenes.empty() &&
	      m_replace_scenes.empty() && m_removingScenes.empty())) {
		// Change the scene list
		ReplaceScheduledScenes();
		RemoveScheduledScenes();
		AddScheduledScenes();
	}

	if (m_scenes->Empty()) {
		RequestExit(KX_ExitInfo::NO_SCENES_LEFT);
	}
}

void KX_KetsjiEngine::SetShowBoundingBox(KX_DebugOption mode)
{
	m_showBoundingBox = mode;
}

KX_DebugOption KX_KetsjiEngine::GetShowBoundingBox() const
{
	return m_showBoundingBox;
}

void KX_KetsjiEngine::SetShowArmatures(KX_DebugOption mode)
{
	m_showArmature = mode;
}

KX_DebugOption KX_KetsjiEngine::GetShowArmatures() const
{
	return m_showArmature;
}

void KX_KetsjiEngine::SetShowCameraFrustum(KX_DebugOption mode)
{
	m_showCameraFrustum = mode;
}

KX_DebugOption KX_KetsjiEngine::GetShowCameraFrustum() const
{
	return m_showCameraFrustum;
}

void KX_KetsjiEngine::SetShowShadowFrustum(KX_DebugOption mode)
{
	m_showShadowFrustum = mode;
}

KX_DebugOption KX_KetsjiEngine::GetShowShadowFrustum() const
{
	return m_showShadowFrustum;
}

void KX_KetsjiEngine::Resize()
{
	/* extended mode needs to recalculate camera frusta when */
	KX_Scene *firstscene = m_scenes->GetFront();
	const RAS_FrameSettings &framesettings = firstscene->GetFramingType();

	if (framesettings.FrameType() == RAS_FrameSettings::e_frame_extend) {
		for (KX_Scene *scene : m_scenes) {
			KX_Camera *cam = scene->GetActiveCamera();
			cam->InvalidateProjectionMatrix();
		}
	}
}

void KX_KetsjiEngine::SetGlobalSettings(GlobalSettings *gs)
{
	m_globalsettings.glslflag = gs->glslflag;
}

GlobalSettings *KX_KetsjiEngine::GetGlobalSettings()
{
	return &m_globalsettings;
}
