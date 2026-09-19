/*=====================================================================
SDLClient.cpp
-------------
Copyright Glare Technologies Limited 2024 -
=====================================================================*/


#include "GUIClient.h"
#include "SDLUIInterface.h"
#if EMSCRIPTEN
#include <settings/EmscriptenSettingsStore.h>
#else
#include <settings/RegistrySettingsStore.h>
#include <settings/XMLSettingsStore.h>
#endif
#include "TestSuite.h"
#include "URLParser.h"
#include "ModelLoading.h"
#include "ImGUIDrawing.h"
#include "CEF.h"
#include <maths/GeometrySampling.h>
#include <graphics/FormatDecoderGLTF.h>
#include <graphics/MeshSimplification.h>
#include <graphics/TextRenderer.h>
#include <graphics/EXRDecoder.h>
#include <opengl/OpenGLEngine.h>
#include <opengl/RenderStatsWidget.h>
#include <opengl/GLMeshBuilding.h>
#include <indigo/TextureServer.h>
#include <opengl/MeshPrimitiveBuilding.h>
#include <utils/Exception.h>
#include <utils/StandardPrintOutput.h>
#include <utils/IncludeWindows.h>
#include <utils/PlatformUtils.h>
#include <utils/FileUtils.h>
#include <utils/ConPrint.h>
#include <utils/Parser.h>
#include <utils/StringUtils.h>
#include <utils/LimitedAllocator.h>
#include <utils/ComObHandle.h>
#include <utils/FileInStream.h>
#include <networking/URL.h>
#include <webserver/Escaping.h>
#include <direct3d/Direct3DUtils.h>
#include <cmath>
#include <GL/gl3w.h>
#include <SDL_opengl.h>
#include <SDL.h>
#include <backends/imgui_impl_opengl3.h>
#include <backends/imgui_impl_sdl2.h>
#include <string>
#if EMSCRIPTEN
#include <emscripten.h>
#include <emscripten/html5.h>
#include <unistd.h>
#include "emscripten_browser_clipboard.h"
#endif
#include <tracy/Tracy.hpp>
#include <tracy/TracyOpenGL.hpp>
#ifdef _WIN32
#include <d3d11.h>
#include <d3d11_4.h>
#include <mfobjects.h>
#endif


// If we are building on Windows, and we are not in Release mode (e.g. BUILD_TESTS is enabled), then make sure the console window is shown.
// Unfortunately the console window does not stay open if no breakpoint is hit.  The only way I know of fixing this is to manually set the 
// subsystem in the VS project settings (Linker > System > SubSystem)
#if defined(_WIN32) && defined(BUILD_TESTS)
#pragma comment(linker, "/SUBSYSTEM:CONSOLE")
#endif

#if !defined(EMSCRIPTEN)
#define EM_BOOL bool
#endif

static void doOneMainLoopIter();


class SDLClientGLUICallbacks : public GLUICallbacks
{
public:
	SDLClientGLUICallbacks() : sys_cursor_arrow(NULL), sys_cursor_Ibeam(NULL) {}

	virtual void startTextInput()
	{
		//conPrint("startTextInput");
		SDL_StartTextInput();
	}

	virtual void stopTextInput()
	{
		//conPrint("stopTextInput");
		SDL_StopTextInput();
	}

	virtual void setMouseCursor(MouseCursor cursor)
	{
		//conPrint("setMouseCursor");
		if(cursor == MouseCursor_Arrow)
		{
			if(!sys_cursor_arrow)
				sys_cursor_arrow = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW);
			SDL_SetCursor(sys_cursor_arrow);
		}
		else if(cursor == MouseCursor_IBeam)
		{
			if(!sys_cursor_Ibeam)
				sys_cursor_Ibeam = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_IBEAM);
			SDL_SetCursor(sys_cursor_Ibeam);
		}
		else
			assert(0);
	}

	SDL_Cursor* sys_cursor_arrow;
	SDL_Cursor* sys_cursor_Ibeam;
};


static void setGLAttribute(SDL_GLattr attr, int value)
{
	const int result = SDL_GL_SetAttribute(attr, value);
	if(result != 0)
	{
		const char* err = SDL_GetError();
		throw glare::Exception("Failed to set OpenGL attribute: " + (err ? std::string(err) : "[Unknown]"));
	}
}


Reference<OpenGLEngine> opengl_engine;

Timer* timer;
Timer* time_since_last_frame;
Timer* stats_timer;
Timer* mem_usage_sampling_timer;
Timer* last_update_URL_timer;
int num_frames = 0;
bool reset = false;
double fps = 0;
bool wireframes = false;

#if defined(_WIN32)
ComObHandle<ID3D11Device> d3d_device;
ComObHandle<IMFDXGIDeviceManager> device_manager;
#endif

SDL_Window* win;
SDL_GLContext gl_context;

float sun_phi = 1.f;
float sun_theta = Maths::pi<float>() / 4;

StandardPrintOutput print_output;
bool quit = false;

GUIClient* gui_client = NULL;
SDLUIInterface* sdl_ui_interface = NULL;

Vec2i mouse_move_origin(0, 0);

static std::vector<float> mem_usage_values;

static bool show_imgui_info_window = false;

// ?glfinish=1.  GL commands only get queued, so timing the draw code measures how long it took to ask for the
// work, not how long the work took - which is why the draw appears to cost a fraction of a millisecond in a
// frame that takes forty.  glFinish is meant to close that gap by waiting for the GPU to drain.
//
// It does not, in a browser.  WebGL treats finish() as advisory and implementations return immediately, so this
// flag makes no measurable difference in the web client and the draw figure stays a submission time.  It is
// kept because it does work in a native build, and because the next person to wonder why the draw looks free
// deserves to find the answer here rather than discovering it again.  Measuring GPU time under WebGL2 needs a
// fence - fenceSync plus a polled clientWaitSync, since the spec forbids a blocking timeout.
static bool sync_gl_for_timing = false;

// ?stereo=1.  Draw the scene twice into one framebuffer, side by side.  See the loop in doOneMainLoopIter().
static bool draw_stereo = false;

// ?xrtest=1.  Paint the world's background bright red for the duration of a session.
//
// This separates the two things a black headset view cannot tell apart.  The engine clears the target itself, at
// the start of its own draw, so if red arrives then the engine's output is reaching the compositor and whatever
// is wrong is in the scene.  If it stays black then nothing the engine draws lands at all, whatever the camera
// and the framebuffer binding say - which is a different fault in a different place.
static bool xr_test_colour = false;

// ?xralt=1.  Alternate between a plain clear and drawing the world, three seconds each.
static bool xr_alternate = false;

// ?xrblit=1.  Draw into a framebuffer of our own and copy it across, rather than drawing into the session's.
static bool xr_use_blit = false;

#if EMSCRIPTEN
// WebXR session state.  Set from JS in webclient.html, which owns the session itself: a session can only be
// started from a user gesture, and the WebXR API is not exposed to Emscripten.
static bool xr_session_active = false;
static unsigned int xr_framebuffer_name = 0; // Emscripten GL name for the opaque framebuffer the session gives us.
static int xr_framebuffer_w = 0, xr_framebuffer_h = 0;
static int xr_frames = 0;
static int xr_last_num_views = 0;
static Reference<FrameBuffer> xr_target_framebuffer;
static unsigned int xr_render_target_name = 0; // Our own framebuffer, which the engine draws into.

// Whether the client started in the cheap render profile.  A headset needs it, and the guess that picks it -
// a device pixel ratio above 1 - is wrong on a Quest, which reports exactly 1.  Recorded so a session can say
// so rather than rendering something unusable.
static bool client_low_memory_mode = false;
static int client_msaa_samples = 1;
static bool xr_saved_render_to_offscreen = false;
static bool xr_saved_draw_overlays = true;
static bool xr_saved_reverse_z = true;
static Colour3f xr_saved_background_colour(0.f);

// Per-view data for a frame, written straight into wasm memory by the session code in webclient.html: sixteen
// floats of projection, sixteen of the world-to-view transform, then four of viewport.  Passing it through a
// shared buffer avoids marshalling forty numbers across the boundary every frame at ninety frames a second.
static const int XR_MAX_VIEWS = 4;
static const int XR_FLOATS_PER_VIEW = 36;
static float xr_view_data[XR_MAX_VIEWS * XR_FLOATS_PER_VIEW];
static Timer* xr_timer = NULL;

// gl3w.h is included below and rewrites these to function pointers it loads itself, which do not exist in an
// Emscripten build.  Reach the GLES entry points directly instead.  Constants are fine - only functions are
// rewritten - so GL_FRAMEBUFFER and friends still come from the header.
extern "C" void emscripten_glBindFramebuffer(unsigned int target, unsigned int framebuffer);
extern "C" void emscripten_glClearColor(float r, float g, float b, float a);
extern "C" void emscripten_glClear(unsigned int mask);
extern "C" void emscripten_glViewport(int x, int y, int w, int h);
extern "C" unsigned int emscripten_glGetError(void);
extern "C" void emscripten_glGetIntegerv(unsigned int pname, int* params);
extern "C" void emscripten_glColorMask(unsigned char r, unsigned char g, unsigned char b, unsigned char a);
extern "C" void emscripten_glDisable(unsigned int cap);
#endif
#if EMSCRIPTEN
extern "C" void emscripten_glFinish(void);
#endif

Reference<RenderStatsWidget> CPU_render_stats_widget;
Reference<RenderStatsWidget> GPU_render_stats_widget;


static double cur_canvas_css_W = 800; // Current device-independent pixel width.  Canvas element css width in WebGL.
static double cur_canvas_css_H = 800;

#if EMSCRIPTEN

// Define getLocationHost() function
EM_JS(char*, getLocationHost, (), {
	return stringToNewUTF8(window.location.host);
});

// Define getLocationPathname() function
EM_JS(char*, getLocationPathname, (), {
	return stringToNewUTF8(window.location.pathname);
});

// Define getLocationSearch() function
// Get the ?a=b part of the URL (see https://developer.mozilla.org/en-US/docs/Web/API/Location)
EM_JS(char*, getLocationSearch, (), {
	return stringToNewUTF8(window.location.search);
});

// Define updateURL(const char* new_URL) function
EM_JS(void, updateURL, (const char* new_URL), {
	history.replaceState(null, "",  UTF8ToString(new_URL)); // See https://developer.mozilla.org/en-US/docs/Web/API/History/replaceState
});

// Publish the frame time breakdown where the page can read it.
//
// The split between work done before the draw and the draw itself is what separates a frame limited by pixels
// from one limited by work that happens whatever the resolution.  The info window shows both, but reading it on
// a headset means aiming at a checkbox, so the numbers go to the page as well and the ?fps=1 overlay shows them.
EM_JS(void, publishFrameTimings, (double cpu_ms, double gl_ms, double client_fps), {
	window.__substrata_frame_timings = { cpu_ms: cpu_ms, gl_ms: gl_ms, client_fps: client_fps };
});

// Render into a framebuffer of our own and copy the result into the session's.
//
// The engine's output is correct - it can be read back from an ordinary framebuffer and shows proper stereo -
// but drawing directly into the session's framebuffer stops the compositor presenting anything at all, and does
// so permanently: a plain clear presents until the first frame the engine draws, and never again afterwards.
// Nothing illegal shows up in a trace of the frame and no GL error is raised.
//
// So the engine is kept away from it.  It draws into a framebuffer we own, which behaves like any other, and a
// single blit copies that across at the end of the frame.  The cost is one full-screen copy; the benefit is that
// the opaque framebuffer is only ever written by one operation whose behaviour is not in doubt.
//
// These are written in JS because creating a framebuffer and registering it in Emscripten's object table is
// already JS work, and doing the blit here too keeps the whole arrangement in one place.
EM_JS(int, createXRRenderTarget, (int w, int h), {
	var gl = GLctx;
	var tex = gl.createTexture();
	gl.bindTexture(gl.TEXTURE_2D, tex);
	gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, w, h, 0, gl.RGBA, gl.UNSIGNED_BYTE, null);
	gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
	gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);

	var depth = gl.createRenderbuffer();
	gl.bindRenderbuffer(gl.RENDERBUFFER, depth);
	gl.renderbufferStorage(gl.RENDERBUFFER, gl.DEPTH_COMPONENT24, w, h);

	var fb = gl.createFramebuffer();
	gl.bindFramebuffer(gl.FRAMEBUFFER, fb);
	gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, tex, 0);
	gl.framebufferRenderbuffer(gl.FRAMEBUFFER, gl.DEPTH_ATTACHMENT, gl.RENDERBUFFER, depth);
	var ok = gl.checkFramebufferStatus(gl.FRAMEBUFFER) === gl.FRAMEBUFFER_COMPLETE;
	gl.bindFramebuffer(gl.FRAMEBUFFER, null);
	if(!ok) return 0;

	var id = GL.getNewId(GL.framebuffers);
	fb.name = id;
	GL.framebuffers[id] = fb;
	return id;
});

EM_JS(void, blitXRRenderTarget, (int src_name, int dst_name, int w, int h), {
	var gl = GLctx;
	gl.bindFramebuffer(gl.READ_FRAMEBUFFER, GL.framebuffers[src_name]);
	gl.bindFramebuffer(gl.DRAW_FRAMEBUFFER, GL.framebuffers[dst_name]);
	gl.blitFramebuffer(0, 0, w, h, 0, 0, w, h, gl.COLOR_BUFFER_BIT, gl.NEAREST);
	gl.bindFramebuffer(gl.READ_FRAMEBUFFER, null);
});

// Publish WebXR session statistics where the flat page can show them.  A headset is not a place to read a
// debug overlay, so the numbers have to survive the session and be readable afterwards.
// A failure inside a session is invisible: the console is on a machine the wearer cannot see, and the headset
// shows only the result.  So anything that goes wrong is recorded where the flat page can show it afterwards.
EM_JS(void, publishXRError, (const char* msg), {
	window.__substrata_xr_error = UTF8ToString(msg);
});

// Not a failure, just what the client saw - shown beside the statistics so a session can be inspected after the
// fact rather than guessed at.
EM_JS(void, publishXRDebug, (const char* msg), {
	window.__substrata_xr_debug = UTF8ToString(msg);
});

EM_JS(void, publishXRStats, (double fps, int frames, int views, int fb_w, int fb_h, int active), {
	window.__substrata_xr = { fps: fps, frames: frames, views: views, fb_w: fb_w, fb_h: fb_h, active: !!active };
});

// Define getUserAgentString() function
EM_JS(char*, getUserAgentString, (), {
	return stringToNewUTF8(window.navigator.userAgent);
});

// From https://groups.google.com/g/angleproject/c/0ZuTYrgaXYw/m/UNdsgYLLCgAJ
EM_JS(char*, getTranslatedShaderSource, (int32_t nm), {
	var ext = GLctx.getExtension('WEBGL_debug_shaders');
	if (ext) {
		if (GL.shaders[nm]) {
			var jsString = ext.getTranslatedShaderSource(GL.shaders[nm]);
			var lengthBytes = lengthBytesUTF8(jsString) + 1;
			var stringOnWasmHeap = _malloc(lengthBytes);
			stringToUTF8(jsString, stringOnWasmHeap, lengthBytes);
			return stringOnWasmHeap;
		}
		else {
			return 0;
		}
	}
	else {
		return 0;
	}
});

#endif


int main(int argc, char** argv)
{
	try
	{
		GUIClient::staticInit();


		std::map<std::string, std::vector<ArgumentParser::ArgumentType> > syntax;
		syntax["--test"] = std::vector<ArgumentParser::ArgumentType>(); // Run unit tests
		syntax["-h"] = std::vector<ArgumentParser::ArgumentType>(1, ArgumentParser::ArgumentType_string); // Specify hostname to connect to
		syntax["-u"] = std::vector<ArgumentParser::ArgumentType>(1, ArgumentParser::ArgumentType_string); // Specify server URL to connect to
		syntax["-linku"] = std::vector<ArgumentParser::ArgumentType>(1, ArgumentParser::ArgumentType_string); // Specify server URL to connect to, when a user has clicked on a substrata URL hyperlink.
		syntax["--extractanims"] = std::vector<ArgumentParser::ArgumentType>(2, ArgumentParser::ArgumentType_string); // Extract animation data
		syntax["--screenshotslave"] = std::vector<ArgumentParser::ArgumentType>(); // Run GUI as a screenshot-taking slave.
		syntax["--testscreenshot"] = std::vector<ArgumentParser::ArgumentType>(); // Test screenshot taking
		syntax["--no_MDI"] = std::vector<ArgumentParser::ArgumentType>(); // Disable MDI in graphics engine
		syntax["--no_bindless"] = std::vector<ArgumentParser::ArgumentType>(); // Disable bindless textures in graphics engine

		std::vector<std::string> args;
		for(int i=0; i<argc; ++i)
			args.push_back(argv[i]);

		if(args.size() == 3 && args[1] == "-NSDocumentRevisionsDebugMode")
			args.resize(1); // This is some XCode debugging rubbish, remove it

		ArgumentParser parsed_args(args, syntax);

#if !defined(EMSCRIPTEN)
		if(parsed_args.isArgPresent("--test"))
		{
			TestSuite::test();
			return 0;
		}
#endif


#if defined(EMSCRIPTEN)
		const std::string base_dir = "";
#else
		const std::string base_dir = PlatformUtils::getResourceDirectoryPath();
#endif

		// NOTE: this code is also in MainWindow.cpp
#if defined(_WIN32)
		const std::string font_path       = PlatformUtils::getFontsDirPath() + "/Segoeui.ttf"; // SegoeUI is shipped with Windows 7 onwards: https://learn.microsoft.com/en-us/typography/fonts/windows_7_font_list
		const std::string emoji_font_path = PlatformUtils::getFontsDirPath() + "/Seguiemj.ttf";
#elif defined(__APPLE__)
		const std::string font_path       = "/System/Library/Fonts/SFNS.ttf";
		const std::string emoji_font_path = "/System/Library/Fonts/SFNS.ttf";
#else
		// Linux:
		const std::string font_path       = base_dir + "/data/resources/TruenoLight-E2pg.otf";
		const std::string emoji_font_path = base_dir + "/data/resources/TruenoLight-E2pg.otf"; 
#endif

		TextRendererRef text_renderer = new TextRenderer();

		TextRendererFontFaceSizeSetRef fonts       = new TextRendererFontFaceSizeSet(text_renderer, font_path);
		TextRendererFontFaceSizeSetRef emoji_fonts = new TextRendererFontFaceSizeSet(text_renderer, emoji_font_path);


		timer = new Timer();
		time_since_last_frame = new Timer();
		stats_timer = new Timer();
		mem_usage_sampling_timer = new Timer();
		last_update_URL_timer = new Timer();

		const std::string appdata_path = PlatformUtils::getOrCreateAppDataDirectory("Cyberspace");

#if EMSCRIPTEN
		Reference<EmscriptenSettingsStore> settings_store = new EmscriptenSettingsStore();
#elif defined(_WIN32)
		Reference<RegistrySettingsStore> settings_store = new RegistrySettingsStore("Glare Technologies", "Cyberspace");
#else
		Reference<XMLSettingsStore> settings_store = new XMLSettingsStore(appdata_path + "/settings_store.xml");
#endif
		


#if EMSCRIPTEN
		const double device_pixel_ratio = emscripten_get_device_pixel_ratio();
#else
		const double device_pixel_ratio = 1.0;
#endif
		printVar(device_pixel_ratio);


		//=========================== Init SDL and OpenGL ================================
		SDL_SetHint(SDL_HINT_EMSCRIPTEN_KEYBOARD_ELEMENT, "#canvas"); // Target the canvas element

		if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0)
			throw glare::Exception("SDL_Init Error: " + std::string(SDL_GetError()));


		// Set GL attributes, needs to be done before window creation.
#if EMSCRIPTEN
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
		setGLAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);

		SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8); // Enable alpha channel for video alpha cutout

#elif defined(__APPLE__)
		setGLAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4); // We need to request a specific version for a core profile.
		setGLAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
		setGLAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
#else
		setGLAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4); // We need to request a specific version for a core profile.
		setGLAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 6);
		setGLAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
#endif

#if EMSCRIPTEN
		// Render quality profile, taken from the page URL: ?gfx=low or ?gfx=high.
		//
		// The guess below infers a mobile device from a device pixel ratio above 1, and a Quest browser defeats
		// it: the headset reports a ratio of 1 and so is handed the full desktop settings - 4x MSAA, bloom,
		// full shadow detail, offscreen render targets - on a mobile-class GPU.  Being able to ask for the
		// cheap profile explicitly is what makes it possible to find out what an XR render profile would buy,
		// without guessing at the device from numbers that do not identify it.
		std::string gfx_profile;
		{
			char* gfx_search_str = getLocationSearch();
			const std::string gfx_search(gfx_search_str);
			free(gfx_search_str);
			if(gfx_search.size() >= 1)
			{
				const std::map<std::string, std::string> gfx_queries = URL::parseQuery(gfx_search.substr(1));

				const auto gfx_res = gfx_queries.find("gfx");
				if(gfx_res != gfx_queries.end())
					gfx_profile = gfx_res->second;

				// ?diag=1 opens the info window at startup.  The native client has F1 for this; a headset has no
				// keyboard, and the two lines it shows - main loop CPU time against updateGL time - are the ones
				// that say whether a frame is limited by pixels or by work that no resolution change will avoid.
				const auto diag_res = gfx_queries.find("diag");
				if((diag_res != gfx_queries.end()) && (diag_res->second == "1"))
					show_imgui_info_window = true;

				const auto sync_res = gfx_queries.find("glfinish");
				if((sync_res != gfx_queries.end()) && (sync_res->second == "1"))
					sync_gl_for_timing = true;

				const auto stereo_res = gfx_queries.find("stereo");
				if((stereo_res != gfx_queries.end()) && (stereo_res->second == "1"))
					draw_stereo = true;

				const auto xrtest_res = gfx_queries.find("xrtest");
				if((xrtest_res != gfx_queries.end()) && (xrtest_res->second == "1"))
					xr_test_colour = true;

				const auto xralt_res = gfx_queries.find("xralt");
				if((xralt_res != gfx_queries.end()) && (xralt_res->second == "1"))
					xr_alternate = true;

				const auto xrblit_res = gfx_queries.find("xrblit");
				if((xrblit_res != gfx_queries.end()) && (xrblit_res->second == "1"))
					xr_use_blit = true;
			}
		}
		conPrint("Graphics profile from URL: '" + gfx_profile + "'");

		// ?gfx=min is ?gfx=low with shadow mapping off as well.  Shadow maps render at their own fixed
		// resolution, so their cost does not fall when the canvas shrinks - which makes them the obvious
		// suspect for any part of a frame that stays the same size whatever the resolution.
		const bool minimal_gfx = (gfx_profile == "min");

		// device_pixel_ratio > 1 is probably a mobile device
		//
		// Stereo forces the cheap profile too.  Drawing several views into one framebuffer needs the offscreen
		// render buffer off: the engine composites that buffer back at the origin, so each view would land on
		// top of the last rather than beside it.  A headset needs these settings regardless, so rather than
		// letting ?stereo=1&gfx=high render half a frame, treat asking for stereo as asking for the profile
		// that can deliver it.
		const bool low_memory_mode = (gfx_profile == "low") || minimal_gfx || draw_stereo ||
			((gfx_profile != "high") && (device_pixel_ratio > 1.0));
#else
		const bool low_memory_mode = false;
#endif
		conPrint("Using low memory mode: " + boolToString(low_memory_mode));
#if EMSCRIPTEN
		client_low_memory_mode = low_memory_mode;
#endif


#if EMSCRIPTEN
		// In theory, since we are not doing hi dpi rendering, we can have MSAA on.
		// However MSAA causes iPad to give context lost errors, so just disable if device_pixel_ratio != 1.
		const bool use_MSAA = !low_memory_mode;
#else
		const bool use_MSAA = settings_store->getBoolValue("setting/MSAA", /*default value=*/true);
#endif
		conPrint("Using MSAA: " + boolToString(use_MSAA));
		if(use_MSAA)
		{
			setGLAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1); // Enable MULTISAMPLE
			setGLAttribute(SDL_GL_MULTISAMPLESAMPLES, 4);
		}
		else
		{
			setGLAttribute(SDL_GL_MULTISAMPLEBUFFERS, 0); // Disable MULTISAMPLE
			setGLAttribute(SDL_GL_MULTISAMPLESAMPLES, 0);
		}

		int primary_W = 1800;
		int primary_H = 1100;

#if EMSCRIPTEN
		// This seems to return the canvas width and height before it is properly sized to the full window width (e.g. is 300x150px), so don't bother calling it.
		// emscripten_get_canvas_element_size("#canvas", &primary_W, &primary_H);
		
		primary_W = 256; // Use small resolution in case these values are used, in which case we don't want to allocate a massive buffer that then gets thrown away.
		primary_H = 256;
#endif

#if EMSCRIPTEN
		const char* window_name = "Substrata Web Client"; // Seems to get used for the web page title
#else
		const char* window_name = "Substrata SDL Client";
#endif
		// SDL_WINDOW_ALLOW_HIGHDPI results in 1:1 drawable (viewport) pixels to device/hardware pixels.
		// This is too high res for mobile - rendering is too slow.  So just leave it off for now.
		win = SDL_CreateWindow(window_name, 600, 100, primary_W, primary_H, SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
		if(win == nullptr)
			throw glare::Exception("SDL_CreateWindow Error: " + std::string(SDL_GetError()));

		cur_canvas_css_W = primary_W;
		cur_canvas_css_H = primary_H;

#if EMSCRIPTEN
		EmscriptenWebGLContextAttributes attrs;
		emscripten_webgl_init_context_attributes(&attrs);

		attrs.premultipliedAlpha = EM_FALSE; // Disable premultiplied alpha, to get desired blending behaviour for our alpha=0 cutout regions for video playing on web.
		attrs.powerPreference = EM_WEBGL_POWER_PREFERENCE_HIGH_PERFORMANCE; // This is required, otherwise we get a "WebGL: lost context" error on iPad.

		EMSCRIPTEN_WEBGL_CONTEXT_HANDLE ctx = emscripten_webgl_create_context("canvas", &attrs);
		if(ctx == 0)
			throw glare::Exception("emscripten_webgl_create_context failed.");

		emscripten_webgl_make_context_current(ctx);
		//gl_context = (SDL_GLContext)ctx;
		//SDL_GL_MakeCurrent(win, gl_context);

		gl_context = SDL_GL_CreateContext(win);
		if(!gl_context)
			throw glare::Exception("OpenGL context could not be created! SDL Error: " + std::string(SDL_GetError()));
#else
		gl_context = SDL_GL_CreateContext(win);
		if(!gl_context)
			throw glare::Exception("OpenGL context could not be created! SDL Error: " + std::string(SDL_GetError()));
#endif


		// SDL_GL_SetSwapInterval(0); // Disable Vsync

		//SDL_SetHint("SDL_HINT_MOUSE_RELATIVE_WARP_MOTION", "true");


		SDL_GameController* game_controller = nullptr;
		if(SDL_NumJoysticks() < 1)
		{
			conPrint("No joysticks / gamepads connected!\n");
		}
		else
		{
			// Load joystick
			game_controller = SDL_GameControllerOpen(/*device index=*/0);
			if(game_controller == nullptr)
				conPrint("Warning: Unable to open game controller! SDL Error: " + std::string(SDL_GetError()));
		}


#if !defined(EMSCRIPTEN)
		gl3wInit();
#endif

		// Create main task manager.
		// This is for doing work like texture compression and EXR loading, that will be created by LoadTextureTasks etc.
		// Alloc these on the heap as Emscripten may have issues with stack-allocated objects before the emscripten_set_main_loop() call.
#if defined(EMSCRIPTEN)
		const size_t main_task_manager_num_threads = myClamp<size_t>(PlatformUtils::getNumLogicalProcessors(), 1, 8);
#else
		const size_t main_task_manager_num_threads = myClamp<size_t>(PlatformUtils::getNumLogicalProcessors(), 1, 512);
#endif
		glare::TaskManager* main_task_manager = new glare::TaskManager("main task manager", main_task_manager_num_threads);
		main_task_manager->setThreadPriorities(MyThread::Priority_Lowest);


		// Create high-priority task manager.
		// For short, processor intensive tasks that the main thread depends on, such as computing animation data for the current frame, or executing Jolt physics tasks.
#if defined(EMSCRIPTEN)
		const size_t high_priority_task_manager_num_threads = myClamp<size_t>(PlatformUtils::getNumLogicalProcessors(), 1, 8);
#else
		const size_t high_priority_task_manager_num_threads = myClamp<size_t>(PlatformUtils::getNumLogicalProcessors(), 1, 512);
#endif
		glare::TaskManager* high_priority_task_manager = new glare::TaskManager("high_priority_task_manager", high_priority_task_manager_num_threads);


		//Reference<glare::Allocator> mem_allocator = new glare::GeneralMemAllocator(/*arena_size_B=*/2 * 1024 * 1024 * 1024ull);
		Reference<glare::Allocator> mem_allocator = new glare::MallocAllocator();
		Reference<glare::Allocator> worker_allocator = new glare::LimitedAllocator(/*max_size_B=*/512 * 1024 * 1024ull);
		//Reference<glare::Allocator> mem_allocator = new glare::MallocAllocator();


		EXRDecoder::setTaskManager(main_task_manager);

		bool on_apple_device = false;
#if defined(EMSCRIPTEN)
		char* user_agent_str = getUserAgentString();
		const std::string user_agent(user_agent_str);
		free(user_agent_str);

		conPrint("user_agent: " + user_agent);
		on_apple_device = StringUtils::containsString(user_agent, "Mac OS") || StringUtils::containsString(user_agent, "iPhone OS");
#else
		#if defined(__APPLE__)
		on_apple_device = true;
		#endif
#endif
		printVar(on_apple_device);


		// Initialise ImGUI
		ImGui::CreateContext();
		ImGui_ImplSDL2_InitForOpenGL(win, gl_context);
		ImGui_ImplOpenGL3_Init();

		// Create OpenGL engine
		OpenGLEngineSettings settings;
		settings.compress_textures = true;
		settings.shadow_mapping = !minimal_gfx;
		settings.depth_fog = true;
		settings.render_water_caustics = !low_memory_mode;
		settings.msaa_samples = use_MSAA ? 4 : 1;
#if EMSCRIPTEN
		client_msaa_samples = settings.msaa_samples;
#endif
		settings.render_to_offscreen_renderbuffers = !low_memory_mode;
		settings.ssao_support = false;
		settings.ssao = false;

		if(parsed_args.isArgPresent("--no_MDI"))
			settings.allow_multi_draw_indirect = false;
		if(parsed_args.isArgPresent("--no_bindless"))
			settings.allow_bindless_textures = false;

		if(on_apple_device)
			settings.use_multiple_phong_uniform_bufs = true; // Work around Mac OpenGL bug with changing the phong uniform buffer between rendering batches (see https://issues.chromium.org/issues/338348430)

#if defined(EMSCRIPTEN)
		settings.max_tex_CPU_mem_usage = 512 * 1024 * 1024ull;
#endif
		if(low_memory_mode)
			settings.shadow_mapping_detail = OpenGLEngineSettings::ShadowMappingDetail_low;

		opengl_engine = new OpenGLEngine(settings);

		
		std::string data_dir = "/data";
#if !defined(EMSCRIPTEN)
		if(PlatformUtils::isEnvironmentVariableDefined("GLARE_CORE_TRUNK_DIR"))
			data_dir = PlatformUtils::getEnvironmentVariable("GLARE_CORE_TRUNK_DIR") + "/opengl";
#endif
		
		opengl_engine->initialise(data_dir, /*texture_server=*/NULL, &print_output, main_task_manager, high_priority_task_manager, mem_allocator);
		if(!opengl_engine->initSucceeded())
			throw glare::Exception("OpenGL init failed: " + opengl_engine->getInitialisationErrorMsg());
		opengl_engine->setViewportDims(primary_W, primary_H);


		sun_phi = 1.f;
		sun_theta = Maths::pi<float>() / 4;
		opengl_engine->setSunDir(normalise(Vec4f(std::cos(sun_phi) * sin(sun_theta), std::sin(sun_phi) * sin(sun_theta), cos(sun_theta), 0)));
		opengl_engine->setEnvMapTransform(Matrix3f::rotationMatrix(Vec3f(0,0,1), sun_phi));




		gui_client = new GUIClient(base_dir, appdata_path, parsed_args);
		gui_client->opengl_engine = opengl_engine;

		// Don't use lightmaps on mobile devices for now, to reduce RAM usage.
		if(low_memory_mode)
			gui_client->use_lightmaps = false;

		std::string cache_dir = appdata_path;


		sdl_ui_interface = new SDLUIInterface();
		sdl_ui_interface->window = win;
		sdl_ui_interface->gl_context = gl_context;
		sdl_ui_interface->gui_client = gui_client;
		sdl_ui_interface->game_controller = game_controller;
		sdl_ui_interface->appdata_path = appdata_path;
		sdl_ui_interface->settings_store = settings_store;
		sdl_ui_interface->d3d11_device = nullptr;


		gui_client->preConnectInitialise(cache_dir, settings_store, sdl_ui_interface, high_priority_task_manager, worker_allocator);


		URLParseResults url_parse_results;
#if EMSCRIPTEN
		// Extract URL details to connect to from from webpage URL
		char* location_host_str = getLocationHost();
		const std::string location_host(location_host_str);
		free(location_host_str);

		url_parse_results.hostname = location_host;

		char* location_path_str = getLocationPathname();
		const std::string location_path(location_path_str);
		free(location_path_str);

		char* search_str = getLocationSearch(); // Get the query part of the URL, will be something like "?world=bleh&x=1&y=2&z=3", or just the empty string
		const std::string search(search_str);
		free(search_str);

		// Parse location path to extract parcel ID if present.
		{
			Parser path_parser(location_path);
			if(path_parser.parseCString("/webclient/"))
			{
			}
			else if(path_parser.parseCString("/visit/"))
			{
			}

			if(path_parser.parseCString("parcel/"))
			{
				int parcel_id;
				if(path_parser.parseInt(parcel_id))
				{
					url_parse_results.parcel_uid = parcel_id;
					url_parse_results.parsed_parcel_uid = true;
				}
			}
		}

		if(search.size() >= 1)
		{
			const std::map<std::string, std::string> queries = URL::parseQuery(search.substr(1)); // Remove '?' prefix from search string, then parse into keys and values.

			URLParser::processQueryKeyValues(queries, url_parse_results);
		}
#else
		std::string server_URL = "sub://substrata.info"; // Default URL

		if(parsed_args.isArgPresent("-h"))
		{
			server_URL = "sub://" + parsed_args.getArgStringValue("-h");
		}
		if(parsed_args.isArgPresent("-u"))
		{
			server_URL = parsed_args.getArgStringValue("-u");
		}

		try
		{
			url_parse_results = URLParser::parseURL(server_URL);
		}
		catch(glare::Exception& e) // Handle URL parse failure
		{
			conPrint(e.what());
			sdl_ui_interface->showPlainTextMessageBox("Error parsing URL", e.what());
			return 1;
		}

#endif
		gui_client->connectToServer(url_parse_results);

		gui_client->postConnectInitialise();

#ifdef _WIN32
		// Create a GPU device.  Needed to get hardware accelerated video decoding and for hardware texture sharing for CEF.
		Direct3DUtils::createGPUDeviceAndMFDeviceManager(d3d_device, device_manager);
		gui_client->device_manager = device_manager.ptr;
		gui_client->d3d_device = d3d_device.ptr;

		sdl_ui_interface->d3d11_device = (void*)d3d_device.ptr;
#endif //_WIN32


		CEF::initialiseCEF(base_dir, appdata_path);

		// NOTE: use 1 for device_pixel_ratio as we are not doing high DPI rendering.
		gui_client->afterGLInitInitialise(/*device_pixel_ratio*/1.f, opengl_engine, fonts, emoji_fonts);

		gui_client->gl_ui->callbacks = new SDLClientGLUICallbacks();

		// A high device_pixel_ratio indicates the UI is going to be very small unless it is scaled up a bit.
		if(device_pixel_ratio > 2.f)
			gui_client->gl_ui->setUIScale((float)device_pixel_ratio / 2.f);





#if EMSCRIPTEN
		// Stop SDL from accepting Ctrl+V events, so that paste events will be properly triggered.
		EM_ASM({
			window.addEventListener('keydown', function(event){
				if (event.ctrlKey && event.key == 'v')
					event.stopImmediatePropagation();
			}, true);
		});

		// Set paste event callback.  The lambda function will execute when a paste action is detected in the browser.
		emscripten_browser_clipboard::paste([](const std::string& paste_data, void* /*callback_data*/){
			TextInputEvent text_input_event;
			text_input_event.text = paste_data;
			gui_client->gl_ui->handleTextInputEvent(text_input_event);
		});
#endif


		//---------------------- Set env material -------------------
		{
			OpenGLMaterial env_mat;
			opengl_engine->setEnvMat(env_mat);
		}
		opengl_engine->setCirrusTexture(opengl_engine->getTexture(base_dir + "/data/resources/cirrus.exr"));


#if EMSCRIPTEN
		// Just disable bloom on mobile devices, is not working properly, probably due to lack of floating point buffer formats or something similar.
		// Keyed off low_memory_mode rather than the pixel ratio directly, so that ?gfx= turns it off with everything else.
		const bool bloom = !low_memory_mode;
#else
		const bool bloom = settings_store->getBoolValue("setting/bloom", /*default val=*/true);
#endif
		conPrint("Bloom enabled: " + boolToString(bloom));
		if(bloom)
			opengl_engine->getCurrentScene()->bloom_strength = 0.3f;

		opengl_engine->getCurrentScene()->draw_aurora = true;

		
		
		conPrint("Starting main loop...");
#if EMSCRIPTEN
		//emscripten_request_animation_frame_loop(doOneMainLoopIter, 0);

		emscripten_set_main_loop(doOneMainLoopIter, /*fps=*/0, /*simulate_infinite_loop=*/true); // fps 0 to use requestAnimationFrame as recommended.
#else
		while(!quit)
		{
			doOneMainLoopIter();
		}
#endif

		conPrint("main finished...");

		CPU_render_stats_widget = NULL;
		GPU_render_stats_widget = NULL;

		gui_client->shutdown();
		delete gui_client;
		gui_client = NULL;

		CEF::shutdownCEF();

		opengl_engine = NULL;

		high_priority_task_manager->waitForTasksToComplete();
		main_task_manager->waitForTasksToComplete();

		EXRDecoder::clearTaskManager(); // Needs to be shut down before main_task_manager is destroyed as uses it.

		delete high_priority_task_manager;
		delete main_task_manager;


		GUIClient::staticShutdown();

		SDL_Quit();
		return 0;
	}
	catch(glare::Exception& e)
	{
		stdErrPrint(e.what());
		return 1;
	}
}


static float sensorWidth() { return 0.035f; }
static float lensSensorDist() { return 0.025f; }


static Vec2f GLCoordsForGLWidgetPos(OpenGLEngine& gl_engine, const Vec2f widget_pos, double device_pixel_ratio)
{
	const int vp_width  = gl_engine.getViewPortWidth();
	const int vp_height = gl_engine.getViewPortHeight();

	const double use_vp_width  = vp_width  / device_pixel_ratio;
	const double use_vp_height = vp_height / device_pixel_ratio;

	return Vec2f(
		(float)( (widget_pos.x - use_vp_width /2) / (use_vp_width /2)),
		(float)(-(widget_pos.y - use_vp_height/2) / (use_vp_height/2))
	);
}


static inline Key getKeyForSDLKey(SDL_Keycode sym)
{
	switch(sym)
	{
		case SDLK_ESCAPE: return Key_Escape;
		case SDLK_BACKSPACE: return Key_Backspace;
		case SDLK_DELETE: return Key_Delete;
		case SDLK_SPACE: return Key_Space;
		case SDLK_RETURN: return Key_Return;
		case SDLK_KP_ENTER: return Key_Enter;
		case SDLK_LEFTBRACKET: return Key_LeftBracket;
		case SDLK_RIGHTBRACKET: return Key_RightBracket;
		case SDLK_PAGEUP: return Key_PageUp;
		case SDLK_PAGEDOWN: return Key_PageDown;
		case SDLK_HOME: return Key_Home;
		case SDLK_END: return Key_End;
		case SDLK_EQUALS: return Key_Equals;
		case SDLK_PLUS: return Key_Plus;
		case SDLK_MINUS: return Key_Minus;
		case SDLK_LEFT: return Key_Left;
		case SDLK_RIGHT: return Key_Right;
		case SDLK_UP: return Key_Up;
		case SDLK_DOWN: return Key_Down;
		default: break;
	};
		
	if(sym >= SDLK_a && sym <= SDLK_z)
		return (Key)(Key_A + (sym - SDLK_a));

	if(sym >= SDLK_0 && sym <= SDLK_9)
		return (Key)(Key_0 + (sym - SDLK_0));

	if(sym >= SDLK_F1 && sym <= SDLK_F12)
		return (Key)(Key_F1 + (sym - SDLK_F1));

	return Key_None;
}


static MouseButton convertSDLMouseButton(uint8 sdl_button)
{
	if(sdl_button == 1)
		return MouseButton::Left;
	else if(sdl_button == 2)
		return MouseButton::Right;
	else if(sdl_button == 3)
		return MouseButton::Middle;
	else
		return MouseButton::None;
}


static uint32 convertSDLMouseButtonState(uint32 sdl_state)
{
	uint32 res = 0;
	if(sdl_state & SDL_BUTTON_LMASK) res |= (uint32)MouseButton::Left;
	if(sdl_state & SDL_BUTTON_MMASK) res |= (uint32)MouseButton::Middle;
	if(sdl_state & SDL_BUTTON_RMASK) res |= (uint32)MouseButton::Right;
	return res;
}


static uint32 convertSDLModifiers(SDL_Keymod sdl_keymod)
{
	uint32 modifiers = 0;
	if((sdl_keymod & SDL_Keymod::KMOD_ALT) != 0)
		modifiers |= Modifiers::Alt;
	if((sdl_keymod & SDL_Keymod::KMOD_CTRL) != 0)
		modifiers |= Modifiers::Ctrl;
	if((sdl_keymod & SDL_Keymod::KMOD_SHIFT) != 0)
		modifiers |= Modifiers::Shift;
	return modifiers;
}


static void convertFromSDKKeyEvent(SDL_Event ev, KeyEvent& key_event)
{
	key_event.key = getKeyForSDLKey(ev.key.keysym.sym);
	key_event.native_virtual_key = 0; // TODO
	// TODO: set key_event.text
	key_event.modifiers = convertSDLModifiers((SDL_Keymod)ev.key.keysym.mod);
}

static void convertFromSDLTextInputEvent(SDL_Event ev, TextInputEvent& text_input_event)
{
	text_input_event.text = std::string(ev.text.text);
}


static size_t last_total_memory = 0;
static uintptr_t last_dynamic_top = 0;

static double last_timerEvent_CPU_work_elapsed = 0;
double last_updateGL_time = 0;

// Sums over the current reporting second, for the figures published to the page.  A single frame's timing is
// quantised to about a millisecond, which is most of the value when the work itself takes one or two; averaging
// over a second's worth of frames recovers the precision.
static double cpu_time_sum = 0;
static double gl_time_sum = 0;
static int timing_sample_count = 0;

static bool doing_cam_rotate_mouse_drag = false; // Is the mouse pointer hidden, and will moving the mouse rotate the camera?
// TODO: replace with ui_interface->getCamRotationOnMouseDragEnabled

static bool have_received_input = false;
static bool tried_initialise_audio_engine = false;


static void doOneMainLoopIter()
{
#if EMSCRIPTEN
	// The XR session drives its own frame loop, so the page loop must not also be drawing.  The loop is
	// cancelled when a session starts; this is here in case a browser delivers one more callback after that.
	if(xr_session_active)
		return;
#endif
	Timer loop_iter_timer;


#ifdef EMSCRIPTEN
	// Web browsers need to wait for an input gesture is completed before trying to play sounds.
	if(have_received_input && !tried_initialise_audio_engine)
	{
		gui_client->initAudioEngine();
		tried_initialise_audio_engine = true;
	}
#endif

	CEF::doMessageLoopWork();


	if(SDL_GL_MakeCurrent(win, gl_context) != 0)
		conPrint("SDL_GL_MakeCurrent failed.");


#if 0 // EMSCRIPTEN // Print when memory size increases
	const size_t total_memory = (size_t)EM_ASM_PTR(return HEAP8.length);
	const uintptr_t dynamic_top = (uintptr_t)sbrk(0);
	if(total_memory != last_total_memory)
	{
		conPrint("************* total_memory increased to " + ::getMBSizeString(total_memory));
		last_total_memory = total_memory;
	}
	if(dynamic_top != last_dynamic_top)
	{
		conPrint("************* dynamic_top increased to " + ::getMBSizeString((size_t)dynamic_top));
		last_dynamic_top = dynamic_top;
	}
#endif

	double canvas_drawable_pixels_per_css_pixels = (double)opengl_engine->getViewPortWidth() / cur_canvas_css_W;

	// Handle any events
	SDL_Event e;
	while(SDL_PollEvent(&e))
	{
		if(show_imgui_info_window)
			ImGui_ImplSDL2_ProcessEvent(&e); // Pass event onto ImGUI

		const bool imgui_captures_mouse_ev    = show_imgui_info_window && ImGui::GetIO().WantCaptureMouse;
		const bool imgui_captures_keyboard_ev = show_imgui_info_window && ImGui::GetIO().WantCaptureKeyboard;

		if(e.type == SDL_QUIT) // "An SDL_QUIT event is generated when the user clicks on the close button of the last existing window" - https://wiki.libsdl.org/SDL_EventType#Remarks
		{
			quit = true;
		}
		else if(e.type == SDL_WINDOWEVENT) // If user closes the window:
		{
			if(e.window.event == SDL_WINDOWEVENT_CLOSE)
			{
				quit = true;
			}
			else if(/*e.window.event == SDL_WINDOWEVENT_RESIZED || */e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED)
			{
				int w, h;
				SDL_GL_GetDrawableSize(win, &w, &h);

				conPrint("Got size changed event, SDL drawable size is " + toString(w) + " x " + toString(h));
						
				opengl_engine->setViewportDims(w, h);

#if EMSCRIPTEN
				emscripten_get_element_css_size("canvas", &cur_canvas_css_W, &cur_canvas_css_H);
				// conPrint("emscripten_get_element_css_size returned " + toString(cur_canvas_css_W) + " x " + toString(cur_canvas_css_H));
#else
				cur_canvas_css_W = w;
				cur_canvas_css_H = h;
#endif

				canvas_drawable_pixels_per_css_pixels = (double)opengl_engine->getViewPortWidth() / cur_canvas_css_W;

				gui_client->gl_ui->setCurrentDevicePixelRatio((float)canvas_drawable_pixels_per_css_pixels); // Use the actual computed device pixel ratio for the canvas element.

				gui_client->viewportResized(w, h); // Do this last so UI will reposition
			}
		}
		else if(e.type == SDL_KEYDOWN)
		{
			if(e.key.keysym.sym == SDLK_F1 || e.key.keysym.sym == SDLK_F2)
				show_imgui_info_window = !show_imgui_info_window;

			if(e.key.keysym.sym == SDLK_F3)
			{
				// Dump out the translated shader source (e.g. ANGLE's HLSL output)
#if EMSCRIPTEN
				for(int i=0; i<100; ++i)
				{
					char* translated_code = getTranslatedShaderSource(i);
					if(translated_code)
					{
						conPrint("shader " + toString(i) + " translated_code:");
						conPrint(std::string(translated_code));
						free(translated_code);
					}
					else
						conPrint("translated_code was NULL");
				}
#endif
			}
			else if(e.key.keysym.sym == SDLK_F5)
			{
#if EMSCRIPTEN
				EM_ASM( window.location.reload(); ); // Just passing F5 on to the browser doesn't trigger a reload.  Need to call this.
				continue;
#endif
			}

			if(!imgui_captures_keyboard_ev)
			{
				KeyEvent key_event;
				convertFromSDKKeyEvent(e, key_event);

				if(key_event.key == Key_X && (key_event.modifiers & (uint32)Modifiers::Ctrl)) // Check for cut command
				{
					//conPrint("CTRL + X detected");
					std::string new_clipboard_contents;
					gui_client->gl_ui->handleCutEvent(new_clipboard_contents);
#if EMSCRIPTEN
					emscripten_browser_clipboard::copy(new_clipboard_contents);
#else
					SDL_SetClipboardText(new_clipboard_contents.c_str());
#endif
				}
				else if(key_event.key == Key_C && (key_event.modifiers & (uint32)Modifiers::Ctrl)) // Check for copy command
				{
					//conPrint("CTRL + C detected");
					std::string new_clipboard_contents;
					gui_client->gl_ui->handleCopyEvent(new_clipboard_contents);
#if EMSCRIPTEN
					emscripten_browser_clipboard::copy(new_clipboard_contents);
#else
					SDL_SetClipboardText(new_clipboard_contents.c_str());
#endif
				}
				else if(key_event.key == Key_V && (key_event.modifiers & (uint32)Modifiers::Ctrl)) // Check for paste command
				{
					TextInputEvent text_input_event;
					char* keyboard_text = SDL_GetClipboardText(); // Caller must call SDL_free() on the returned pointer when done with it
					text_input_event.text = std::string(keyboard_text);
					SDL_free(keyboard_text);

					gui_client->gl_ui->handleTextInputEvent(text_input_event);
				}
				else
					gui_client->keyPressed(key_event);
			}
		}
		else if(e.type == SDL_KEYUP)
		{
			if(!imgui_captures_keyboard_ev)
			{
				KeyEvent key_event;
				convertFromSDKKeyEvent(e, key_event);

				gui_client->keyReleased(key_event);
			}

			have_received_input = true;
		}
		else if(e.type == SDL_TEXTINPUT)
		{
			TextInputEvent text_input_event;
			convertFromSDLTextInputEvent(e, text_input_event);

			gui_client->gl_ui->handleTextInputEvent(text_input_event);
		}
		else if(e.type == SDL_MOUSEMOTION)
		{
			// conPrint("SDL_MOUSEMOTION, pos: " + Vec2i(e.motion.x, e.motion.y).toString());
			if(!imgui_captures_mouse_ev)
			{
				if(doing_cam_rotate_mouse_drag && (e.motion.state & SDL_BUTTON_LMASK))
				{
					Vec2i delta(e.motion.xrel, -e.motion.yrel);

					// conPrint("delta: " + delta.toString());

#if EMSCRIPTEN
					const double speed_factor = 1.0; // This gives similar results in a web browser to the 0.35 below.
#else
					const double speed_factor = 0.35; // To make delta similar to what Qt gives.
#endif
					gui_client->cam_controller.updateRotation(/*pitch_delta=*/delta.y * speed_factor, /*heading_delta=*/delta.x * speed_factor);

					// On Windows/linux, reset the cursor position to where we started, so we never run out of space to move.
					// QCursor::setPos() does not work on mac, and also gives a message about Substrata trying to control the computer, which we want to avoid.
					// So don't use setPos() on Mac.
#if !defined(OSX) && !defined(EMSCRIPTEN)
					SDL_WarpMouseInWindow(win, mouse_move_origin.x, mouse_move_origin.y);
#endif

					SDL_GetMouseState(&mouse_move_origin.x, &mouse_move_origin.y);
				}
			

				MouseEvent move_event;
				move_event.cursor_pos = Vec2i(e.motion.x, e.motion.y);
				move_event.gl_coords = GLCoordsForGLWidgetPos(*opengl_engine, Vec2f((float)e.motion.x, (float)e.motion.y), canvas_drawable_pixels_per_css_pixels);
				move_event.modifiers = convertSDLModifiers(SDL_GetModState());
				move_event.button_state = convertSDLMouseButtonState(e.motion.state);
				gui_client->mouseMoved(move_event);
			}
		}
		else if(e.type == SDL_MOUSEBUTTONDOWN)
		{
			if(!imgui_captures_mouse_ev)
			{
				// conPrint("SDL_MOUSEBUTTONDOWN, pos: " + Vec2i(e.button.x, e.button.y).toString() + ", clicks: " + toString(e.button.clicks));

				SDL_GetMouseState(&mouse_move_origin.x, &mouse_move_origin.y);

				MouseEvent mouse_event;
				mouse_event.cursor_pos = Vec2i(e.button.x, e.button.y);
				mouse_event.gl_coords = GLCoordsForGLWidgetPos(*opengl_engine, Vec2f((float)e.button.x, (float)e.button.y), canvas_drawable_pixels_per_css_pixels);
				mouse_event.button = convertSDLMouseButton(e.button.button);
				mouse_event.modifiers = convertSDLModifiers(SDL_GetModState());

				if(e.button.clicks == 1) // Single click:
				{
					gui_client->mousePressed(mouse_event);
					if(!mouse_event.accepted)
					{
						//conPrint("Entering relative mouse mode to true...");
						SDL_SetRelativeMouseMode(SDL_TRUE); // Hide mouse cursor and constrain to window and report relative mouse motion.
						doing_cam_rotate_mouse_drag = true;
					}
				}
				else if(e.button.clicks == 2) // Double click:
				{
					gui_client->mouseDoubleClicked(mouse_event);
				}
			}
		}
		else if(e.type == SDL_MOUSEBUTTONUP)
		{
			if(!imgui_captures_mouse_ev)
			{
				MouseEvent mouse_event;
				mouse_event.cursor_pos = Vec2i(e.button.x, e.button.y);
				mouse_event.gl_coords = GLCoordsForGLWidgetPos(*opengl_engine, Vec2f((float)e.button.x, (float)e.button.y), canvas_drawable_pixels_per_css_pixels);
				mouse_event.button = convertSDLMouseButton(e.button.button);
				mouse_event.modifiers = convertSDLModifiers(SDL_GetModState());
				gui_client->gl_ui->handleMouseRelease(mouse_event);

				// conPrint("SDL_MOUSEBUTTONUP, pos: " + Vec2i(e.button.x, e.button.y).toString() + ", clicks: " + toString(e.button.clicks));

				if(e.button.clicks != 2) // If not the release from the second click in a double-click (already implicitly handled in mouseDoubleClicked above)
				{
					SDL_SetRelativeMouseMode(SDL_FALSE);
					doing_cam_rotate_mouse_drag = false;
				}
			}

			have_received_input = true;
		}
		else if(e.type == SDL_MOUSEWHEEL)
		{
			if(!imgui_captures_mouse_ev)
			{
				MouseWheelEvent wheel_event;
				wheel_event.cursor_pos = Vec2i(e.wheel.mouseX, e.wheel.mouseY);
				wheel_event.gl_coords = GLCoordsForGLWidgetPos(*opengl_engine, Vec2f((float)e.wheel.mouseX, (float)e.wheel.mouseY), canvas_drawable_pixels_per_css_pixels);
				wheel_event.angle_delta = Vec2f(e.wheel.preciseX, e.wheel.preciseY) * 15.f; // Most mouse wheels have 15 degree increments, preciseY seems to be -1 or 1.
				wheel_event.modifiers = convertSDLModifiers(SDL_GetModState());
				gui_client->onMouseWheelEvent(wheel_event);
			}
		}
	}


	MouseCursorState mouse_cursor_state;
	{
		SDL_GetMouseState(&mouse_cursor_state.cursor_pos.x, &mouse_cursor_state.cursor_pos.y); // Get mouse cursor pos
		mouse_cursor_state.gl_coords = GLCoordsForGLWidgetPos(*opengl_engine, Vec2f((float)mouse_cursor_state.cursor_pos.x, (float)mouse_cursor_state.cursor_pos.y), canvas_drawable_pixels_per_css_pixels);
	
		const SDL_Keymod mod_state = SDL_GetModState();
		mouse_cursor_state.ctrl_key_down = (mod_state & KMOD_CTRL) != 0;
		mouse_cursor_state.alt_key_down  = (mod_state & KMOD_ALT)  != 0;
	}
	
	try
	{
		gui_client->timerEvent(mouse_cursor_state);
	}
	catch(glare::Exception& e)
	{
		conPrint("ERROR: Excep while calling gui_client->timerEvent(): " + e.what());
	}

	if(stats_timer->elapsed() > 1.0)
	{
		// Update statistics
		fps = num_frames / stats_timer->elapsed();
		// conPrint("fps: " + doubleToStringNDecimalPlaces(fps, 1));
		stats_timer->reset();
		num_frames = 0;

#if EMSCRIPTEN
		if(timing_sample_count > 0)
			publishFrameTimings((cpu_time_sum / timing_sample_count) * 1000, (gl_time_sum / timing_sample_count) * 1000, fps);
#endif
		cpu_time_sum = 0;
		gl_time_sum = 0;
		timing_sample_count = 0;
	}

#if TRACE_ALLOCATIONS
	if(mem_usage_sampling_timer->elapsed() > 0.25f)
	{
		mem_usage_sampling_timer->reset();
		mem_usage_values.push_back((float)MemAlloc::getTotalAllocatedB() / (1024 * 1024));
	}
#endif

	last_timerEvent_CPU_work_elapsed = loop_iter_timer.elapsed(); // Everything before graphics draw
	cpu_time_sum += last_timerEvent_CPU_work_elapsed;
	timing_sample_count++;

	Timer drawing_timer;

	int gl_w, gl_h;
	SDL_GL_GetDrawableSize(win, &gl_w, &gl_h);
	if(gl_w > 0 && gl_h > 0)
	{
		// Work out current camera transform
		Matrix4f world_to_camera_space_matrix;
		gui_client->cam_controller.getWorldToCameraMatrix(world_to_camera_space_matrix);

		const float near_draw_dist = 0.22f;
		const float max_draw_dist = 100000.f;

		const float sensor_width = sensorWidth();
		const float lens_sensor_dist = (float)gui_client->cam_controller.lens_sensor_dist;

		opengl_engine->setNearDrawDistance(near_draw_dist);
		opengl_engine->setMaxDrawDistance(max_draw_dist);

		// ?stereo=1 draws the scene twice, side by side in one framebuffer, each eye into its own half.
		//
		// This is the shape a WebXR frame has, minus WebXR: two views sharing a framebuffer, each addressed by a
		// viewport offset.  It is here to prove the offset is honoured - a split view is either right down the
		// middle or obviously wrong - and to put a number on what a second view costs, which the single-view
		// measurements could not.  Both eyes use the same projection, so this is not a correct stereo image; it
		// is the cost and the plumbing, not the optics.
		const int num_views = draw_stereo ? 2 : 1;
		for(int view = 0; view < num_views; ++view)
		{
			const int view_w = gl_w / num_views;

			if(draw_stereo)
				opengl_engine->setViewportRect(view * view_w, 0, view_w, gl_h);
			else
				opengl_engine->setViewportDims(view_w, gl_h);

			// The first view clears the whole framebuffer; the rest do not clear at all.  See
			// setViewIndexInFrame() - a clear confined to one eye is expensive on a tile-based GPU.
			opengl_engine->setViewIndexInFrame(view);

			Matrix4f view_matrix = world_to_camera_space_matrix;
			if(draw_stereo)
			{
				// Shift the camera along its own x axis by half an interpupillary distance, one way per eye.
				const float half_IPD = 0.032f;
				const float shift = (view == 0) ? half_IPD : -half_IPD;
				view_matrix = Matrix4f::translationMatrix(shift, 0, 0) * view_matrix;
			}

			const float render_aspect_ratio = (float)view_w / (float)gl_h;
			opengl_engine->setPerspectiveCameraTransform(view_matrix, sensor_width, lens_sensor_dist, render_aspect_ratio, /*lens shift up=*/0.f, /*lens shift right=*/0.f);
			opengl_engine->draw();
		}

		// Leave the viewport covering the whole target, so anything drawn afterwards is not confined to one eye.
		if(draw_stereo)
		{
			opengl_engine->setViewportDims(gl_w, gl_h);
			opengl_engine->setViewIndexInFrame(0);
		}
	}

	if(show_imgui_info_window)
	{
		// Draw ImGUI GUI controls
		ImGui_ImplOpenGL3_NewFrame();
		ImGui_ImplSDL2_NewFrame();
		ImGui::NewFrame();

		gui_client->buildImGuiContent(last_timerEvent_CPU_work_elapsed, last_updateGL_time); // Draw the window contents shared with the Qt client.  See ImGUIDrawing.cpp.

#if TRACE_ALLOCATIONS
		if(ImGui::Begin("Memory"))
		{
			ImGui::TextUnformatted("mem usage (MB)");
			ImGui::PlotLines("mem usage", mem_usage_values.data(), (int)mem_usage_values.size(),
					/*values offset=*/0, /* overlay text=*/NULL,
				/*scale min=*/0.0, /*scale max=*/std::numeric_limits<float>::max(),
				/*graph size=*/ImVec2(500, 200));
		}
		ImGui::End();
#endif

		ImGui::Render();
		ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

		// Create or destroy the render stats widgets, to match the 'show frame time graphs' checkbox in the ImGUI window.
		const bool show_frame_time_graphs = gui_client->imgui_drawing->show_frame_time_graphs;
		if(show_frame_time_graphs && CPU_render_stats_widget.isNull())
		{
			opengl_engine->setProfilingEnabled(true);

			CPU_render_stats_widget = new RenderStatsWidget(opengl_engine, gui_client->gl_ui, /*widget index=*/0);
			GPU_render_stats_widget = new RenderStatsWidget(opengl_engine, gui_client->gl_ui, /*widget index=*/1);
		}
		else if(!show_frame_time_graphs && CPU_render_stats_widget.nonNull())
		{
			opengl_engine->setProfilingEnabled(false);

			CPU_render_stats_widget = nullptr;
			GPU_render_stats_widget = nullptr;
		}
	}

	// Plot the total time spent on CPU work this frame.
	// Note that we can't just measure the time of this timerEvent method with glWidget->updateGL(), because updateGL() will block for vsync, so it will include a lot of waiting time.
	// Instead use the sum of work time in this method plus the work time in OpenGLEngine::draw().
	if(CPU_render_stats_widget)
		CPU_render_stats_widget->addFrameTime((float)(last_timerEvent_CPU_work_elapsed + opengl_engine->last_draw_CPU_time));

	if(GPU_render_stats_widget)
		GPU_render_stats_widget->addFrameTime((float)opengl_engine->last_total_draw_GPU_time);
	
	// Display
	SDL_GL_SwapWindow(win);
	FrameMark; // Tracy profiler

	if(sync_gl_for_timing)
	{
		// Block until the GPU has finished, so the time below is execution and not just submission.
		//
		// gl3w.h is included unconditionally above and rewrites glFinish to its own loaded function pointer,
		// which does not exist in an Emscripten build - so reach the GLES entry point directly here.
#if EMSCRIPTEN
		emscripten_glFinish();
#else
		glFinish();
#endif
	}

	last_updateGL_time = drawing_timer.elapsed();
	gl_time_sum += last_updateGL_time;

	time_since_last_frame->reset();

	

#if EMSCRIPTEN
	// Update URL with current camera position
	// We can't do this too often or we will get an "Attempt to use history.replacestate() more than 100 times per 30 seconds" error in Safari.
	if(last_update_URL_timer->elapsed() > 0.5)
	{
		const std::string url_path = gui_client->getCurrentWebClientURLPath();
	
		updateURL(url_path.c_str());

		last_update_URL_timer->reset();
	}
#endif

	num_frames++;
}


static std::string sanitiseString(const std::string& s)
{
	std::string res = s;
	for(size_t i=0; i<s.size(); ++i)
		if(!::isAlphaNumeric(s[i]))
			res[i] = '_';
	return res;
}


#if EMSCRIPTEN


// The three functions below are called from the WebXR code in webclient.html.  See the scope notes in
// ROADMAP.md: this is phase one, the session lifecycle.  Nothing of the world is drawn yet.  What it does prove,
// and what everything after it depends on, is that the opaque framebuffer the session hands over can be reached
// from here at all - Emscripten addresses GL objects by integer name through a table of its own, and WebXR
// hands out a JavaScript object, so the two have to be introduced.  Clearing that framebuffer to a colour that
// moves is the smallest thing that demonstrates the whole chain works from inside the headset.


extern "C" EMSCRIPTEN_KEEPALIVE
float* xrViewBuffer()
{
	return xr_view_data;
}


// WebXR's camera space is x right, y up, z backwards.  The engine's is x right, y forwards, z up.  This maps one
// to the other - (x, y, z) becomes (x, -z, y) - and serves twice over: once to turn the headset's view transform
// into the engine's convention, and once to place the session's reference space in the z-up world.
static const Matrix4f xrToWorldBasis()
{
	return Matrix4f(Vec4f(1, 0, 0, 0), Vec4f(0, 0, 1, 0), Vec4f(0, -1, 0, 0), Vec4f(0, 0, 0, 1));
}


static const Matrix4f worldToXRBasis()
{
	return Matrix4f(Vec4f(1, 0, 0, 0), Vec4f(0, 0, -1, 0), Vec4f(0, 1, 0, 0), Vec4f(0, 0, 0, 1));
}


extern "C" EMSCRIPTEN_KEEPALIVE
void xrSessionStarted(unsigned int framebuffer_name, int fb_width, int fb_height)
{
	conPrint("xrSessionStarted: framebuffer name " + toString(framebuffer_name) + ", " + toString(fb_width) + " x " + toString(fb_height));

	if(!client_low_memory_mode || (client_msaa_samples > 1))
	{
		const std::string msg = "started in the expensive render profile (MSAA " + toString(client_msaa_samples) +
			"), which a headset may not be able to use - reload with ?gfx=low";
		publishXRError(msg.c_str());
	}

	xr_framebuffer_name = framebuffer_name;
	xr_framebuffer_w = fb_width;
	xr_framebuffer_h = fb_height;
	xr_frames = 0;
	xr_session_active = true;

	delete xr_timer;
	xr_timer = new Timer();

	// Wrap the session's framebuffer so the engine draws into it.  FrameBuffer's GLuint constructor does not
	// take ownership, which is what is wanted here: the framebuffer belongs to the session.
	// ?xrblit=1 only.  It does not fix the black view - draw() disables the session's presentation wherever it
	// draws, including into a framebuffer of our own - so it costs a full-screen copy for nothing and is kept
	// only because it isolates the engine from the session's framebuffer, which is worth having while the cause
	// is still open.
	xr_render_target_name = xr_use_blit ? (unsigned int)createXRRenderTarget(fb_width, fb_height) : framebuffer_name;
	if(xr_use_blit && xr_render_target_name == 0)
	{
		publishXRError("could not create a render target for the session");
		xr_render_target_name = framebuffer_name; // Fall back to drawing straight into the session's.
	}

	xr_target_framebuffer = new FrameBuffer(xr_render_target_name);
	opengl_engine->setTargetFrameBuffer(xr_target_framebuffer);

	// The session's framebuffer is default-like: its colour output is not COLOR_ATTACHMENT0, and naming that as
	// the draw buffer stops it accepting colour at all.  A framebuffer of our own, as ?xrblit=1 uses, is an
	// ordinary one and does use attachments.
	opengl_engine->setTargetFrameBufferUsesAttachments(xr_use_blit);

	OpenGLScene* scene = opengl_engine->getCurrentScene();

	// Both eyes share one framebuffer, which rules out the offscreen render path: that buffer is allocated at
	// the viewport's size and composited back at the origin, so the second eye would land on top of the first.
	xr_saved_render_to_offscreen = scene->render_to_main_render_framebuffer;
	scene->render_to_main_render_framebuffer = false;

	// The interface draws in screen space, which in a headset is wrong rather than merely ugly.  Phase four
	// gives it a place in the world; until then it is better absent than floating on the eye.
	xr_saved_draw_overlays = scene->draw_overlay_objects;
	scene->draw_overlay_objects = false;

	// Turn reverse-z off for the session.
	//
	// Reverse-z depends on clearing the depth buffer to zero and testing with GREATER.  A WebXR session's
	// framebuffer does not honour that clear: the runtime clears its depth to one at the start of each frame and
	// a clear to zero from here does not take, so every fragment fails the test and the world renders black -
	// with no GL error, correct output when the same frame is drawn into an ordinary framebuffer, and colour
	// clears working perfectly.  Demonstrated directly on the headset: a triangle at depth 0.5 draws with the
	// depth test off, and disappears with GREATER against a depth buffer cleared to zero.
	//
	// Conventional depth agrees with what the runtime leaves in the buffer, so it simply works.
	xr_saved_reverse_z = opengl_engine->use_reverse_z;
	opengl_engine->use_reverse_z = false;

	if(xr_test_colour)
	{
		xr_saved_background_colour = scene->background_colour;
		scene->background_colour = Colour3f(1.f, 0.f, 0.f);
	}

	emscripten_cancel_main_loop(); // Stop the page's frame loop.  The session drives frames from now on.
}


extern "C" EMSCRIPTEN_KEEPALIVE
void xrFrame(int num_views)
{
	if(!xr_session_active)
		return;

	xr_frames++;
	if(num_views > 0)
		xr_last_num_views = num_views;

	// Drain anything left pending before touching GL, so a stale error is not attributed to this frame.  The
	// engine raises an INVALID_ENUM every frame in ordinary rendering as well: its texture unit indices run past
	// the limit WebGL2 commonly provides, which is harmless while the passes that would use those units are off.
	while(emscripten_glGetError() != 0) {}

	emscripten_glBindFramebuffer(GL_FRAMEBUFFER, xr_framebuffer_name);

	if(num_views <= 0)
	{
		// No pose this frame - tracking has not settled, or the headset is off the face.  Normal, and briefly.
		// Clear so the compositor is not handed the last frame again, and skip the scene.
		emscripten_glViewport(0, 0, xr_framebuffer_w, xr_framebuffer_h);
		emscripten_glClearColor(0.f, 0.f, 0.f, 1.f);
		emscripten_glClear(GL_COLOR_BUFFER_BIT);
	}
	else
	{
		// ?xrtest=1 paints the whole framebuffer blue here, before the engine touches it, while the world's
		// background is set to red for the session.  What comes back names the fault:
		//
		//   the world   everything works
		//   red         the engine's clear lands but its geometry does not
		//   blue        nothing the engine does lands, though a direct clear from here does
		//   black       not even a direct clear lands, and phase one's result no longer holds
		if(xr_test_colour)
		{
			emscripten_glViewport(0, 0, xr_framebuffer_w, xr_framebuffer_h);
			emscripten_glClearColor(0.f, 0.f, 1.f, 1.f);
			emscripten_glClear(GL_COLOR_BUFFER_BIT);
		}

		// ?xralt=1 alternates every three seconds between clearing the framebuffer to blue and drawing the
		// world, so a single look says which of the two the compositor is willing to present.  Phase one, which
		// only ever cleared, did present; the world does not; this puts both in one session with nothing else
		// changing between them.
		const bool clear_only_now = xr_alternate && (((int)(xr_timer ? xr_timer->elapsed() : 0.0) / 3) % 2 == 0);
		if(clear_only_now)
		{
			emscripten_glBindFramebuffer(GL_FRAMEBUFFER, xr_framebuffer_name);
			emscripten_glViewport(0, 0, xr_framebuffer_w, xr_framebuffer_h);
			emscripten_glClearColor(0.f, 0.3f, 1.f, 1.f);
			emscripten_glClear(GL_COLOR_BUFFER_BIT);
		}

		const int use_num_views = clear_only_now ? 0 : myMin(num_views, XR_MAX_VIEWS);

		try
		{

		// The session's reference space sits at the player's position in the world.  Where the head is within
		// that space comes from the headset; where the space itself is comes from the ordinary camera
		// controller, which is what movement will continue to drive.
		const Vec4f player_pos = gui_client->cam_controller.getPosition().toVec4fPoint();
		const Matrix4f world_to_ref_space = Matrix4f::translationMatrix(-player_pos[0], -player_pos[1], -player_pos[2]);

		const Matrix4f to_engine_basis = xrToWorldBasis();
		const Matrix4f from_engine_basis = worldToXRBasis();

		for(int view = 0; view < use_num_views; ++view)
		{
			const float* const v = xr_view_data + (view * XR_FLOATS_PER_VIEW);
			const float* const proj = v;          // 16 floats, column major
			const float* const world_to_view = v + 16; // 16 floats, column major
			const float* const viewport = v + 32;      // x, y, width, height

			const int vp_x = (int)viewport[0], vp_y = (int)viewport[1];
			const int vp_w = (int)viewport[2], vp_h = (int)viewport[3];
			if((vp_w <= 0) || (vp_h <= 0))
				continue;

			// Recover the frustum from the projection the runtime handed us, and express it in the engine's
			// sensor-and-lens terms.  For a projection matrix in the usual form, the half extents at unit
			// distance are 1/m0 and 1/m5, and the asymmetry - which is what makes this a per-eye projection
			// rather than a centred one - is m8/m0 and m9/m5.  Those map exactly onto lens shift, which the
			// engine already has and already accounts for when culling.
			const float m0 = proj[0], m5 = proj[5], m8 = proj[8], m9 = proj[9];
			if((m0 == 0.f) || (m5 == 0.f))
				continue;

			const float half_width_at_unit_dist = 1.f / m0;
			const float unit_shift_right        = m8 / m0;
			const float unit_shift_up           = m9 / m5;

			const float lens_sensor_dist = 1.f; // Free choice: only ratios against it matter.
			const float sensor_width     = 2.f * half_width_at_unit_dist * lens_sensor_dist;

			// world_to_camera = basis * (world to view, in XR terms) * inverse basis * (world to reference space)
			const Matrix4f world_to_view_xr(world_to_view);
			const Matrix4f world_to_camera = to_engine_basis * world_to_view_xr * from_engine_basis * world_to_ref_space;

			opengl_engine->setViewportRect(vp_x, vp_y, vp_w, vp_h);
			opengl_engine->setViewIndexInFrame(view);

			// Render aspect equal to the viewport's keeps the engine from adjusting the sensor size to fit,
			// so the frustum stays exactly the one the runtime asked for.
			const float aspect = (float)vp_w / (float)vp_h;
			opengl_engine->setPerspectiveCameraTransform(world_to_camera, sensor_width, lens_sensor_dist, aspect,
				/*lens shift up=*/unit_shift_up * lens_sensor_dist, /*lens shift right=*/unit_shift_right * lens_sensor_dist);

			opengl_engine->draw();
			}

			// A GL error inside a session is otherwise completely silent: the draw simply does not land and the
			// headset shows black, which is what an empty framebuffer looks like too.  Report the codes raised
			// by this frame's drawing, now that the pre-existing ones have been drained.
			// Every distinct code, not just the first.  Reporting only the first hid anything that followed
			// behind the INVALID_ENUM the engine raises every frame regardless - which is exactly the sort of
			// thing that would be masking the real fault here.
			unsigned int codes[8]; int code_counts[8]; int num_codes = 0;
			unsigned int e; int total = 0;
			while((e = emscripten_glGetError()) != 0)
			{
				bool found = false;
				for(int i=0; i<num_codes; ++i)
					if(codes[i] == e) { code_counts[i]++; found = true; break; }
				if(!found && (num_codes < 8)) { codes[num_codes] = e; code_counts[num_codes] = 1; num_codes++; }
				if(++total > 256) break;
			}
			if(num_codes > 0)
			{
				std::string msg = "GL errors drawing views:";
				for(int i=0; i<num_codes; ++i)
					msg += " " + toString(codes[i]) + "x" + toString(code_counts[i]);
				msg += "  (1280 x2 is expected: activeTexture past the unit limit)";
				publishXRError(msg.c_str());
			}

			// Whether the scene went where it was meant to.  If the engine has left some other framebuffer bound
			// by the end of the frame, the drawing landed somewhere the compositor will never show - which looks
			// exactly like drawing nothing.
			if((xr_frames % 90) == 1)
			{
				int bound_draw_fb = 0, bound_read_fb = 0;
				emscripten_glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &bound_draw_fb);
				emscripten_glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &bound_read_fb);

				// Where the engine believes the camera is, which is the thing a black view cannot distinguish
				// from a correct one.  If this is somewhere sensible and the view is still black, the fault is
				// after the camera; if it is not, the fault is in the transform built above.
				const Vec4f cam_pos = opengl_engine->getCameraPositionWS();
				const Vec4f cam_fwd = opengl_engine->getCurrentScene()->cam_to_world.getColumn(1);

				const std::string msg = "fb " + toString(bound_draw_fb) + "/" + toString(xr_framebuffer_name) +
					" views " + toString(use_num_views) +
					" cam " + doubleToStringNDecimalPlaces(cam_pos[0], 1) + "," + doubleToStringNDecimalPlaces(cam_pos[1], 1) + "," + doubleToStringNDecimalPlaces(cam_pos[2], 1) +
					" fwd " + doubleToStringNDecimalPlaces(cam_fwd[0], 2) + "," + doubleToStringNDecimalPlaces(cam_fwd[1], 2) + "," + doubleToStringNDecimalPlaces(cam_fwd[2], 2);
				publishXRDebug(msg.c_str());
			}

			// Copy what the engine drew into the session's framebuffer.
			if(xr_render_target_name != xr_framebuffer_name)
				blitXRRenderTarget(xr_render_target_name, xr_framebuffer_name, xr_framebuffer_w, xr_framebuffer_h);

			// Force alpha to one across the whole framebuffer.
			//
			// A headset compositor reads the alpha channel; a canvas on a page largely does not, which is why a
			// scene that looks right in the browser can arrive in a headset as nothing at all.  The engine
			// leaves alpha wherever its materials happened to put it, so rather than trust that, set it.
			// Colour is masked off, so this touches nothing that was drawn.
			emscripten_glBindFramebuffer(GL_FRAMEBUFFER, xr_framebuffer_name);
			emscripten_glDisable(GL_SCISSOR_TEST);
			emscripten_glViewport(0, 0, xr_framebuffer_w, xr_framebuffer_h);
			emscripten_glColorMask(0, 0, 0, 1);
			emscripten_glClearColor(0.f, 0.f, 0.f, 1.f);
			emscripten_glClear(GL_COLOR_BUFFER_BIT);
			emscripten_glColorMask(1, 1, 1, 1);
		}
		catch(glare::Exception& e)
		{
			publishXRError(("drawing a view threw: " + e.what()).c_str());
		}
		catch(std::exception& e)
		{
			publishXRError((std::string("drawing a view threw: ") + e.what()).c_str());
		}
	}

	// Publish unconditionally.  An earlier version skipped this when no time had passed, to avoid dividing by
	// zero, which meant the statistics silently did not appear at all if the timer had not ticked yet - and the
	// timer's resolution here is about a millisecond.  A frame rate of zero is a worse answer than no answer,
	// but no answer at all looks like the session is broken.
	const double elapsed = xr_timer ? xr_timer->elapsed() : 0.0;
	publishXRStats((elapsed > 0.001) ? (xr_frames / elapsed) : 0.0, xr_frames, num_views, xr_framebuffer_w, xr_framebuffer_h, /*active=*/1);
}


extern "C" EMSCRIPTEN_KEEPALIVE
void xrSessionEnded()
{
	const double elapsed = xr_timer ? xr_timer->elapsed() : 0.0;
	conPrint("xrSessionEnded: " + toString(xr_frames) + " frames in " + doubleToStringNDecimalPlaces(elapsed, 1) + " s");

	publishXRStats((elapsed > 0.001) ? (xr_frames / elapsed) : 0.0, xr_frames, xr_last_num_views, xr_framebuffer_w, xr_framebuffer_h, /*active=*/0);

	xr_session_active = false;
	xr_framebuffer_name = 0;

	opengl_engine->setTargetFrameBuffer(NULL); // Back to the page's canvas.
	opengl_engine->setTargetFrameBufferUsesAttachments(true);
	xr_target_framebuffer = NULL;

	OpenGLScene* scene = opengl_engine->getCurrentScene();
	scene->render_to_main_render_framebuffer = xr_saved_render_to_offscreen;
	scene->draw_overlay_objects = xr_saved_draw_overlays;
	opengl_engine->use_reverse_z = xr_saved_reverse_z;
	if(xr_test_colour)
		scene->background_colour = xr_saved_background_colour;
	opengl_engine->setViewportDims(opengl_engine->getViewPortWidth(), opengl_engine->getViewPortHeight()); // Clears the offset.
	opengl_engine->setViewIndexInFrame(0);

	emscripten_set_main_loop(doOneMainLoopIter, /*fps=*/0, /*simulate_infinite_loop=*/false); // Resume the page's loop.
}


#endif // EMSCRIPTEN


// processFilePickerFile is called from JS code in webclient.html.
extern "C" 
#if EMSCRIPTEN
EMSCRIPTEN_KEEPALIVE
#endif
void processFilePickerFile(unsigned char* data, int length, const char* filename_, int pick_type_id)
{
	const std::string filename(filename_);

	// conPrint("In C++ processFilePickerFile: " + toString(length) + ", filename: '" + filename + "', pick_type_id: " + toString(pick_type_id));

	// conPrint("--Content--");
	// conPrint(std::string(data, data + length));
	// conPrint("--end Content--");

	try
	{
		if(pick_type_id == UIInterface::PICK_ANIMATION_FILE)
		{
			if(!filename.empty())
			{
				// Save to a temporary file
				const std::string local_temp_path = "/tmp/" + sanitiseString(removeDotAndExtension(filename)) + "." + getExtension(filename);
				FileUtils::writeEntireFile(local_temp_path, (const char*)data, length);
			
				gui_client->handleAnimationFilePickedFromEmscripten(local_temp_path);
			}
			else
				gui_client->showErrorNotification("Please choose a file");
		}
		else if(pick_type_id == UIInterface::PICK_AVATAR_MODEL)
		{
			// Empty filename is acceptable, in that case we use the default Xbot model.
			std::string model_path;
			if(!filename.empty())
			{
				// Save to a temporary file
				model_path = "/tmp/" + sanitiseString(removeDotAndExtension(filename)) + "." + getExtension(filename);
				FileUtils::writeEntireFile(model_path, (const char*)data, length);
			}

			ModelLoading::MakeGLObjectResults results;
			if(!model_path.empty())
			{
				ModelLoading::makeGLObjectForModelFile(*gui_client->opengl_engine, *gui_client->opengl_engine->vert_buf_allocator, /*allocator=*/nullptr, model_path, /*do_opengl_stuff=*/false,
					results
				);
			}
			else
			{
				// We will be using the default Xbot model
				results.ob_to_world = Matrix4f::rotationAroundXAxis(Maths::pi_2<float>()); // If we used the default xbot bmesh we need to rotate it upright.
			
				results.materials.resize(2);
				results.materials[0] = new WorldMaterial();
				results.materials[0]->colour_rgb = Avatar::defaultMat0Col();
				results.materials[0]->metallic_fraction.val = Avatar::default_mat0_metallic_frac;
				results.materials[0]->roughness.val = Avatar::default_mat0_roughness;

				results.materials[1] = new WorldMaterial();
				results.materials[1]->colour_rgb = Avatar::defaultMat1Col();
				results.materials[1]->metallic_fraction.val = Avatar::default_mat1_metallic_frac;
			}

			// NOTE: a bunch of this code is duplicated from AvatarSettingsDialog.

			Vec4f original_toe_pos = results.batched_mesh ? results.batched_mesh->animation_data.getNodePositionModelSpace("LeftToe_End", /*use_retarget_adjustment=*/false) : Vec4f(0,0.0362269469f,0,1);

			// TEMP: Load animation data for ready-player-me type avatars
			float foot_bottom_height = original_toe_pos[1] - 0.0362269469f; // Should be ~= 0

			if(results.batched_mesh)
			{
				// Do retargetting on a copy as we don't want to send the animation data with all animations from extracted_avatar_anim.bin to the server when uploading the mesh.
				AnimationData animation_data_copy = results.batched_mesh->animation_data;

				animation_data_copy.loadAndRetargetAnim(*gui_client->animation_manager.getAnimation("Idle.subanim", *gui_client->resource_manager));

				Vec4f new_toe_pos = animation_data_copy.getNodePositionModelSpace("LeftToe_End", /*use_retarget_adjustment=*/true);
				conPrint("new_toe_pos: " + new_toe_pos.toStringNSigFigs(4));

				foot_bottom_height = new_toe_pos[1] - 0.03f; // Height of foot bottom for avatar with retargetted animation, off ground.

				conPrint("foot_bottom_height: " + doubleToStringNSigFigs(foot_bottom_height, 4));
			}


			// Construct transformation to bring ready-player-me avatars to z-up and standing on the ground.
			// We want to translate the avatar down from 1.67 metres in the sky (which is the default substrata eye height), to the ground
			const Matrix4f pre_ob_to_world_matrix = Matrix4f::translationMatrix(0, 0, -1.67f - foot_bottom_height) * results.ob_to_world;

			// conPrint("processAvatarModelFile(): pre_ob_to_world_matrix: " + pre_ob_to_world_matrix.toString());

			gui_client->updateOurAvatarModel(results.batched_mesh, model_path, pre_ob_to_world_matrix, results.materials);
		}
		else
			throw glare::Exception("processFilePickerFile(): Invalid/unhandled pick_type_id: " + toString(pick_type_id));
	}
	catch(glare::Exception& e)
	{
		conPrint("Excep: " + e.what());

		gui_client->showErrorNotification(e.what());
	}
}
