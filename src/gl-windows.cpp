#include <gl-windows.h>

#include <assert.h>
#include <windows.h>

#include <glad/glad.h>
#include <glad/glad_wgl.h>

#include <vector>
#include <iostream>
#include <iomanip> // std::setw
#include <string>
#include <thread>
#include <chrono>

const char* glerr2str(GLenum errorCode) {
	switch(errorCode) {
		default:
			return "unknown error code";
		case GL_NO_ERROR:
			return "no error";
		case GL_INVALID_ENUM:
			return "invalid enumerant";
		case GL_INVALID_VALUE:
			return "invalid value";
		case GL_INVALID_OPERATION:
			return "invalid operation";
		#ifndef GL_VERSION_3_0
		case GL_STACK_OVERFLOW:
			return "stack overflow";
		case GL_STACK_UNDERFLOW:
			return "stack underflow";
		case GL_TABLE_TOO_LARGE:
			return "table too large";
		#endif
		case GL_OUT_OF_MEMORY:
			return "out of memory";
		#ifdef GL_EXT_framebuffer_object
		case GL_INVALID_FRAMEBUFFER_OPERATION_EXT:
			return "invalid framebuffer operation";
		#endif
	}
}

#define GL(opname, ...) gl ## opname (__VA_ARGS__); { GLenum e = glGetError(); if (e!=GL_NO_ERROR) { \
	printf("%s %s %s %s %d %s %s\n", glerr2str(e), "returned by", #opname, "at line", __LINE__, "in file", __FILE__); } }

// Holds if GL is enabled through createGLContexts
static bool initialized_ = false;

// Holds initial window size
static unsigned int width_ = 500;
static unsigned int height_ = 500;

// Holds mouse coords in pixels, origin = top left
static unsigned int mouse_[2];
static bool mouseDown_ = false;
static char pressedKey_ = '\0';

// Holds the Windows window handle
static HWND windowHandle_;

// Holds the Windows GL context handle
static HGLRC glRenderContext_;

// Holds GLSL version used in shaders
static const std::string glslVersionString_ = "#version 430\n";

// Holds constant vertex shader source
static const std::string vertexShaderSource_ = glslVersionString_ +
"layout(location = 0) in vec2 pos;\
layout(location=42) uniform vec2 scale = vec2(1);\
layout(location=43) uniform vec2 translation = vec2(0);\
out vec2 p;\
void main() {\
	gl_Position = vec4(pos*scale+translation, 0, 1);\
	p = pos;\
}";

// Flag through which main thread shows that input thread should end
static bool isRunning_ = false;

// Flag showing when uncompiled input is ready
static bool inputThreadFlag_ = false;

void runREPL();
static std::thread inputThread(runREPL);

// Holds number of images added through createGLImage or createGLFramebuffer, maps directly to used texture units
static unsigned int imageCount_ = 0;

/**
* ViewState is an internal management structure to enable multiple views in the GL window.
* Views can be switched with PAGE[UP/DOWN] keys. This will also change the current shader in the console.
* A stack model is implemented, so that a view can access the view below via shader textures.
* This works by rendering the lower view into an offscreen framebuffer in first pass, and current view in second pass.
* Use pushGLView() followed by createGLQuad() to e.g. postprocess underlying view in shader of new view.
*/
struct ViewState {
	GLuint vao_ = 0;

	// Holds GL shader handles
	GLuint vertexShader_ = 0;
	GLuint fragmentShader_ = 0;
	GLuint shaderProgram_ = 0;

	// Holds declarations of currently added buffers, samplers etc.
	std::string glslUniformString_ = "";

	// Holds fragment shader code, that can be extended via input at runtime
	std::string fragmentShaderSource_ =
"in vec2 p;\n\
out vec4 color;\n\
int i; float f;\n\
void main() {\n\
  color = vec4(p, 0, 1);\n";

	// Holds a copy of the above after input was made, gets written back when successfully compiled
	std::string fragmentShaderSourceTmp_;

	// Holds local image counter per view/shader, needed to set uniforms
	unsigned int imageCount_ = 0;
	int imageOffset_ = -1;

	// Holds number of vertices added through createGLTriangles2D
	GLsizei currentVertexCount_ = 0;

	// Holds current drawing primitive
	GLenum currentPrimitive_ = GL_TRIANGLES;

	float vertexScale_[2] = { 1.0f, 1.0f };
	float vertexTranslation_[2] = { 0.0f, 0.0f };

	GLuint framebuffer_;

} defaultView_;

static std::vector<ViewState> viewStates_{ defaultView_ };
static unsigned int activeView_ = 0;
static unsigned int prevActiveView_ = 0;




using namespace std::chrono;
static duration<double, std::milli> shaderTime_;
static duration<double, std::milli> frameTime_;
static high_resolution_clock::time_point launchTime_;
static high_resolution_clock::time_point lastSwapTime_;
static uint64_t frameCount_ = 0;



/**
*
*/
static bool compileGLShader(std::string vertSrc, std::string fragSrc, GLuint* outVertexShader, GLuint* outFragmentShader) {
	const auto vertStr = vertSrc.c_str();
	const auto fragStr = fragSrc.c_str();
	const int vertStrLen = (int)vertSrc.length();
	const int fragStrLen = (int)fragSrc.length();
	GLuint v = glCreateShader(GL_VERTEX_SHADER);
	GLuint f = glCreateShader(GL_FRAGMENT_SHADER);
	glShaderSource(v, 1, &vertStr, &vertStrLen);
	glShaderSource(f, 1, &fragStr, &fragStrLen);
	glCompileShader(f);
	glCompileShader(v);
	GLint success;
	glGetShaderiv(f, GL_COMPILE_STATUS, &success);
	if (success != GL_TRUE) {
		GLchar errorString[1024];
		glGetShaderInfoLog(f, 1024, 0, errorString);
		std::cout << std::endl << errorString;
		return false;
	}
	*outVertexShader = v;
	*outFragmentShader = f;
	return true;
}

/**
* Create framebuffer with readable color and depth textures attached
*/
static void createGLFramebuffer(GLuint* f) {

	GLint viewport[4];
	glGetIntegerv(GL_VIEWPORT, viewport);
	GLsizei w = viewport[2], h = viewport[3];

	GL(GenFramebuffers, 1, f);
	GL(BindFramebuffer, GL_FRAMEBUFFER, *f);

	GLint maxUnits; glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &maxUnits);
	if (imageCount_ >= (unsigned int)maxUnits) {
		std::cout << "Only " << maxUnits << " texture units are guaranteed by GL" << std::endl;
		GL(DrawBuffer, GL_NONE);
		GL(ReadBuffer, GL_NONE);
		return;
	}

	ViewState* v = &viewStates_[activeView_];

	// Add shader code to access the texture later
	const std::string name = "view" + std::to_string(activeView_ - 1);
	const std::string countStr = std::to_string(imageCount_);
	v->glslUniformString_ += "layout(location = " + countStr + ") uniform sampler2D " + name + ";\n";
	GL(ActiveTexture, GL_TEXTURE0 + imageCount_);

	// Attach color buffer texture (RGBA8)
	GLuint cbuffer;
	GL(GenTextures, 1, &cbuffer);
	GL(BindTexture, GL_TEXTURE_2D, cbuffer);
	GL(TexStorage2D, GL_TEXTURE_2D, 1, GL_RGBA8, w, h);
	GL(FramebufferTexture2D, GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, cbuffer, 0);
	GL(DrawBuffer, GL_COLOR_ATTACHMENT0);
	GL(ReadBuffer, GL_COLOR_ATTACHMENT0);

	// Texture access mode
	GL(TexParameteri, GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	GL(TexParameteri, GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	GL(TexParameteri, GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	GL(TexParameteri, GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
	GL(TexParameteri, GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);

	// Analog for the depth texture
	if (v->imageOffset_ < 0) v->imageOffset_ = imageCount_;
	imageCount_++; v->imageCount_++;
	const std::string nameD = "view" + std::to_string(activeView_ - 1) + "_depth";
	const std::string countStrD = std::to_string(imageCount_);
	v->glslUniformString_ += "layout(location = " + countStrD + ") uniform sampler2D " + nameD + ";\n";
	GL(ActiveTexture, GL_TEXTURE0 + imageCount_);
	imageCount_++; v->imageCount_++;

	// Attach z buffer texture (R32F)
	GLuint zbuffer;
	GL(GenTextures, 1, &zbuffer);
	GL(BindTexture, GL_TEXTURE_2D, zbuffer);
	GL(TexStorage2D, GL_TEXTURE_2D, 1, GL_DEPTH_COMPONENT32F, w, h);
	GL(FramebufferTexture2D, GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, zbuffer, 0);

	GL(TexParameteri, GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	GL(TexParameteri, GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	GL(TexParameteri, GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	GL(TexParameteri, GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
	GL(TexParameteri, GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);

	// Check
	GLenum status;
	if (status = glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		switch (status) {
		case GL_FRAMEBUFFER_UNDEFINED:
			printf("GL_FRAMEBUFFER_UNDEFINED"); break;
		case GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT:
			printf("GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT"); break;
		case GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT:
			printf("GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT"); break;
		case GL_FRAMEBUFFER_INCOMPLETE_DRAW_BUFFER:
			printf("GL_FRAMEBUFFER_INCOMPLETE_DRAW_BUFFER"); break;
		case GL_FRAMEBUFFER_INCOMPLETE_READ_BUFFER:
			printf("GL_FRAMEBUFFER_INCOMPLETE_READ_BUFFER"); break;
		case GL_FRAMEBUFFER_UNSUPPORTED:
			printf("GL_FRAMEBUFFER_UNSUPPORTED"); break;
		case GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE:
			printf("GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE"); break;
		case GL_FRAMEBUFFER_INCOMPLETE_LAYER_TARGETS:
			printf("GL_FRAMEBUFFER_INCOMPLETE_LAYER_TARGETS"); break;
		default:
			printf("Framebuffer incomplete");
		}
		exit(1);
	}
}

/**
* Callback for system messages (e.g. user input or other events)
* Also the application can send messages (e.g. via UpdateWindow, DispatchMessage)
* More info:
* https://docs.microsoft.com/en-us/windows/desktop/winmsg/about-messages-and-message-queues
*/
static LRESULT CALLBACK onWindowMessage(
	HWND windowHandle,
	UINT uMsg,        // message identifier
	WPARAM wParam,    // first message parameter
	LPARAM lParam)    // second message parameter
{ 
	switch (uMsg) 
	{ 
		case WM_CREATE: 
			// Initialize the window. 
			return 0; 
		case WM_SIZE: 
			// Set the size and position of the window.
			width_ = LOWORD(lParam);
			height_ = HIWORD(lParam);
			return 0; 

		case WM_CHAR:
			if (wParam == VK_ESCAPE) {
				PostQuitMessage(0);
			}
			return 0;
		case WM_KEYDOWN:
			if (wParam == VK_PRIOR) {
				activeView_ = (activeView_ + 1) % viewStates_.size();
			}
			if (wParam == VK_NEXT) {
				activeView_ = (activeView_ == 0 ? viewStates_.size() - 1 : activeView_ - 1) % viewStates_.size();
			}
			pressedKey_ = (char)wParam;
			return 0;
		case WM_KEYUP:
			pressedKey_ = '\0';
			return 0;

		case WM_LBUTTONDOWN:
			mouseDown_ = true;
			mouse_[0] = LOWORD(lParam);
			mouse_[1] = HIWORD(lParam);
			return 0;
		case WM_MOUSEMOVE:
			mouse_[0] = LOWORD(lParam);
			mouse_[1] = HIWORD(lParam);
			return 0;
		case WM_LBUTTONUP:
			mouseDown_ = false;
			mouse_[0] = LOWORD(lParam);
			mouse_[1] = HIWORD(lParam);
			return 0;

		case WM_DESTROY: 
			// Clean up window-specific data objects. 
			PostQuitMessage(0); // puts a WM_QUIT message on the message queue, causing the message loop to end.
			return 0; 

		// 
		// Process other messages. 
		// 

		case WM_PAINT:
			// Paint the window's client area.
			// Redraw the window when it has been minimized, maximized, 
			// or another window is on top of it and things of that sort.
			// We dont rely on WM_PAINT since we continuously render at 60Hz.
		default: 
			return DefWindowProc(windowHandle, uMsg, wParam, lParam); 
	} 
	return 0; 
}

/**
*
*/
static void createWindowsWindow(const char* title, int w, int h, HWND* outWindowHandle) {
	// Handle of exe or dll, depending on where call happens
	HINSTANCE moduleHandle = (HINSTANCE)GetModuleHandle(NULL);

	WNDCLASSEX wcx;
	// Fill in the window class structure with parameters 
	// that describe the main window. 
	wcx.cbSize = sizeof(wcx);          // size of structure 
	wcx.style = CS_OWNDC;              // Allocates a unique device context (DC) for each window in the class. 
		// https://docs.microsoft.com/en-us/windows/desktop/gdi/about-device-contexts
	wcx.lpfnWndProc = onWindowMessage; // points to window procedure 
	wcx.cbClsExtra = 0;                // no extra class memory 
	wcx.cbWndExtra = 0;                // no extra window memory 
	wcx.hInstance = moduleHandle;      // handle to instance 
	wcx.hIcon = LoadIcon(NULL, 
		IDI_APPLICATION);              // predefined app. icon 
	wcx.hCursor = LoadCursor(NULL, 
		IDC_ARROW);                    // predefined arrow 
	wcx.hbrBackground = static_cast<HBRUSH>(GetStockObject( 
		WHITE_BRUSH));                  // white background brush 
	wcx.lpszMenuName =  "MainMenu";    // name of menu resource 
	wcx.lpszClassName = "MainWClass";  // name of window class 
	wcx.hIconSm = static_cast<HICON>(LoadImage(moduleHandle, // small class icon 
		MAKEINTRESOURCE(5),
		IMAGE_ICON, 
		GetSystemMetrics(SM_CXSMICON), 
		GetSystemMetrics(SM_CYSMICON), 
		LR_DEFAULTCOLOR)); 

	// Register the window class. 
	RegisterClassEx(&wcx);

	// Now createWindow can be called
	HWND windowHandle = CreateWindow( 
		"MainWClass",        // name of window class 
		title,               // title-bar string 
		WS_OVERLAPPEDWINDOW, // top-level window 
		CW_USEDEFAULT,       // default horizontal position 
		CW_USEDEFAULT,       // default vertical position 
		w,                   // width 
		h,                   // height 
		(HWND) NULL,         // no owner window 
		(HMENU) NULL,        // use class menu 
		moduleHandle,        // handle to application instance 
		(LPVOID) NULL);      // no window-creation data
	*outWindowHandle = windowHandle;
}

/**
* Declarations of the first GL functions we need to call before we can even load functions that create the context
*/
typedef HGLRC (__stdcall * PFNWGLCREATECONTEXTATTRIBSARB) (HDC hDC, 
	HGLRC hShareContext, const int *attribList);
typedef BOOL (__stdcall * PFNWGLCHOOSEPIXELFORMATARBPROC)(HDC hdc, 
	const int * piAttribIList, const FLOAT * pfAttribFList, UINT nMaxFormats, int * piFormats, UINT * nNumFormats);

/**
* This is copied from Cinder
*/
static HWND createDummyWindow() {
	DWORD windowExStyle, windowStyle;

	WNDCLASS	wc;						// Windows Class Structure
	RECT		WindowRect;				// Grabs Rectangle Upper Left / Lower Right Values
	WindowRect.left = 0L;
	WindowRect.right = 640L;
	WindowRect.top = 0L;
	WindowRect.bottom = 480L;

	HINSTANCE instance	= ::GetModuleHandle( NULL );				// Grab An Instance For Our Window
	wc.style			= CS_HREDRAW | CS_VREDRAW | CS_OWNDC;	// Redraw On Size, And Own DC For Window.
	wc.lpfnWndProc		= DefWindowProc;						// WndProc Handles Messages
	wc.cbClsExtra		= 0;									// No Extra Window Data
	wc.cbWndExtra		= 0;									// No Extra Window Data
	wc.hInstance		= instance;
	wc.hIcon			= ::LoadIcon( NULL, IDI_WINLOGO );		// Load The Default Icon
	wc.hCursor			= ::LoadCursor( NULL, IDC_ARROW );		// Load The Arrow Pointer
	wc.hbrBackground	= NULL;									// No Background Required For GL
	wc.lpszMenuName		= NULL;									// We Don't Want A Menu
	wc.lpszClassName	= TEXT("FLINTTEMP");

	if( ! ::RegisterClass( &wc ) ) {											// Attempt To Register The Window Class
		DWORD err = ::GetLastError();
		return 0;											
	}
	windowExStyle = WS_EX_APPWINDOW | WS_EX_WINDOWEDGE | WS_EX_ACCEPTFILES;		// Window Extended Style
	windowStyle = ( WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME );

	::AdjustWindowRectEx( &WindowRect, windowStyle, FALSE, windowExStyle );

	return ::CreateWindowEx( windowExStyle, TEXT("FLINTTEMP"), TEXT("FLINT"), windowStyle, 0, 0, 
		WindowRect.right-WindowRect.left, WindowRect.bottom-WindowRect.top, NULL, NULL, instance, 0 );
}

/**
* Copied from Cinder
*/
static bool getWglFunctionPointers(
	PFNWGLCREATECONTEXTATTRIBSARB *resultCreateContextAttribsFnPtr, 
	PFNWGLCHOOSEPIXELFORMATARBPROC *resultChoosePixelFormatFnPtr)
{
	static PFNWGLCREATECONTEXTATTRIBSARB cachedCreateContextAttribsFnPtr = nullptr;
	static PFNWGLCHOOSEPIXELFORMATARBPROC cachedChoosePixelFormatFnPtr = nullptr;
	if( ! cachedCreateContextAttribsFnPtr || ! cachedChoosePixelFormatFnPtr ) {
		static PIXELFORMATDESCRIPTOR pfd = {
			sizeof(PIXELFORMATDESCRIPTOR),				// Size Of This Pixel Format Descriptor
			1,											// Version Number
			PFD_DRAW_TO_WINDOW |						// Format Must Support Window
			PFD_SUPPORT_OPENGL |						// Format Must Support OpenGL
			PFD_DOUBLEBUFFER,							// Must Support Double Buffering
			PFD_TYPE_RGBA,								// Request An RGBA Format
			32,											// Select Our Color Depth
			0, 0, 0, 0, 0, 0,							// Color Bits Ignored
			0,											// No Alpha Buffer
			0,											// Shift Bit Ignored
			0,											// No Accumulation Buffer
			0, 0, 0, 0,									// Accumulation Bits Ignored
			16,											// depth bits
			0,											// stencil bits
			0,											// No Auxiliary Buffer
			PFD_MAIN_PLANE,								// Main Drawing Layer
			0,											// Reserved
			0, 0, 0										// Layer Masks Ignored
		};

		HWND tempWindow = createDummyWindow();
		HDC tempDc = ::GetDC( tempWindow );
		auto pixelFormat = ::ChoosePixelFormat( tempDc, &pfd );
		if( pixelFormat == 0 ) {
			::ReleaseDC( tempWindow, tempDc );
			::DestroyWindow( tempWindow );
			::UnregisterClass( TEXT("FLINTTEMP"), ::GetModuleHandle( NULL ) );
			return false;
		}
		::SetPixelFormat( tempDc, pixelFormat, &pfd );
		auto tempCtx = ::wglCreateContext( tempDc ); 
		::wglMakeCurrent( tempDc, tempCtx );

		cachedCreateContextAttribsFnPtr = (PFNWGLCREATECONTEXTATTRIBSARB) ::wglGetProcAddress( "wglCreateContextAttribsARB" );
		cachedChoosePixelFormatFnPtr = (PFNWGLCHOOSEPIXELFORMATARBPROC) ::wglGetProcAddress( "wglChoosePixelFormatARB" );
		*resultCreateContextAttribsFnPtr = cachedCreateContextAttribsFnPtr;
		*resultChoosePixelFormatFnPtr = cachedChoosePixelFormatFnPtr;
		::wglMakeCurrent( NULL, NULL );
		::wglDeleteContext( tempCtx );

		::ReleaseDC( tempWindow, tempDc );
		::DestroyWindow( tempWindow );
		::UnregisterClass( TEXT("FLINTTEMP"), ::GetModuleHandle( NULL ) );

		if( ! cachedCreateContextAttribsFnPtr || ! cachedChoosePixelFormatFnPtr ) {
			return false;
		}
		else
			return true;
	}
	else {
		*resultCreateContextAttribsFnPtr = cachedCreateContextAttribsFnPtr;
		*resultChoosePixelFormatFnPtr = cachedChoosePixelFormatFnPtr;
		return cachedCreateContextAttribsFnPtr && cachedChoosePixelFormatFnPtr;
	}
}

/**
*
*/
static void createGLContext(HDC deviceContext, HGLRC* outGLContext) {
	// https://www.khronos.org/opengl/wiki/Creating_an_OpenGL_Context_(WGL)
	PIXELFORMATDESCRIPTOR pfd = {
		sizeof(PIXELFORMATDESCRIPTOR),
		1,
		PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER,    // Flags
		PFD_TYPE_RGBA,        // The kind of framebuffer. RGBA or palette.
		32,                   // Colordepth of the framebuffer.
		0, 0, 0, 0, 0, 0,
		0,
		0,
		0,
		0, 0, 0, 0,
		24,                   // Number of bits for the depthbuffer
		8,                    // Number of bits for the stencilbuffer
		0,                    // Number of Aux buffers in the framebuffer.
		PFD_MAIN_PLANE,
		0,
		0, 0, 0
	};
	int pixelFormat = ChoosePixelFormat(deviceContext, &pfd);
	SetPixelFormat(deviceContext, pixelFormat, &pfd);

	// https://www.khronos.org/registry/OpenGL/extensions/ARB/WGL_ARB_create_context.txt
	PFNWGLCREATECONTEXTATTRIBSARB wglCreateContextAttribsARBPtr = NULL;
	PFNWGLCHOOSEPIXELFORMATARBPROC wglChoosePixelFormatARBPtr = NULL;
	assert( getWglFunctionPointers( &wglCreateContextAttribsARBPtr, &wglChoosePixelFormatARBPtr ) );
	int attribList[] = {
		WGL_CONTEXT_MAJOR_VERSION_ARB, 4,
		WGL_CONTEXT_MINOR_VERSION_ARB, 0,
		WGL_CONTEXT_FLAGS_ARB, 0,
		WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
		0, 0
	};
	HGLRC glContext = wglCreateContextAttribsARBPtr(deviceContext, 0, attribList);

	wglMakeCurrent(deviceContext, glContext);
	*outGLContext = glContext;
}

/**
*
*/
static void loadGLFunctions() {
	gladLoadGL();
	// wglGetProcAddress();
}

/**
* Check inputThreadFlag_ and compile the new code.
*/
void hotreloadGLShader() {
	// Check if new shader code ready
	if (inputThreadFlag_) {
		ViewState* v = &viewStates_[activeView_];
		GLuint newVertexShader, newFragmentShader;
		if (compileGLShader(vertexShaderSource_,
			glslVersionString_ + v->glslUniformString_ + v->fragmentShaderSourceTmp_ + "}",
			&newVertexShader, &newFragmentShader)) 
		{
			// Workaround to print current source after view state change
			// (mysteriously input thread didnt have input previously appended to this source)
			//if (v->fragmentShaderSource_ == v->fragmentShaderSourceTmp_)
			//	std::cout << '\n' << v->fragmentShaderSourceTmp_;

			v->fragmentShaderSource_ = v->fragmentShaderSourceTmp_;
			glDeleteShader(v->fragmentShader_);
			glDeleteShader(v->vertexShader_);
			glDeleteProgram(v->shaderProgram_);
			v->vertexShader_ = newVertexShader;
			v->fragmentShader_ = newFragmentShader;
			v->shaderProgram_ = glCreateProgram();
			glAttachShader(v->shaderProgram_, v->fragmentShader_);
			glAttachShader(v->shaderProgram_, v->vertexShader_);
			glLinkProgram(v->shaderProgram_);
			std::cout << "  [OK]" << std::endl;
		} else {
			std::cout << "  [CONTINUE AFTER ERROR]" << std::endl;
		}
		std::cout << "  " << std::flush; // indentation
		inputThreadFlag_ = false;
	}
}

/**
*
*/
static void clearConsole(char fill = ' ') {
	COORD tl = { 0,0 };
	CONSOLE_SCREEN_BUFFER_INFO s;
	HANDLE console = GetStdHandle(STD_OUTPUT_HANDLE);
	GetConsoleScreenBufferInfo(console, &s);
	DWORD written, cells = s.dwSize.X * s.dwSize.Y;
	FillConsoleOutputCharacter(console, fill, cells, tl, &written);
	FillConsoleOutputAttribute(console, s.wAttributes, cells, tl, &written);
	SetConsoleCursorPosition(console, tl);
}

/**
*
*/
static void printREPLIntro() {
	ViewState* v = &viewStates_[activeView_];
	std::cout << " ____________________________________________________________" << std::endl;
	std::cout << "|::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::|" << std::endl;
	std::cout << "|:::::::::::::::::: FRAGMENT SHADER EDITOR ::::::::::::::::::|" << std::endl;
	std::cout << "|::::::: works only in Microsoft shell, ESC to close ::::::::|" << std::endl;
	std::cout << std::endl;
}

/**
* Polls stdin for GLSL code while isRunning_ is true.
* Sets inputThreadFlag_ when a line of code is ready to be compiled.
*/
static void runREPL() {
	while (!isRunning_) {
		//do nothing, thread is not meant to start yet
	}
	ViewState* v = &viewStates_[activeView_];
	printREPLIntro();
	std::cout << glslVersionString_ + v->glslUniformString_ + v->fragmentShaderSource_;
	std::cout << "  " << std::flush; // indentation

	const int pollInterval = 200; // milliseconds
	std::string in;
	while (isRunning_) {

		if (activeView_ != prevActiveView_) {
			prevActiveView_ = activeView_;
			v = &viewStates_[activeView_];
			clearConsole();
			printREPLIntro();
			std::cout << glslVersionString_ + v->glslUniformString_ + v->fragmentShaderSource_;
			std::cout << "  " << in << std::flush; // indentation
			in = ""; // discard unfinished input
		}

		if (in.size() == 0) {
			duration<double> sec = std::chrono::high_resolution_clock::now() - launchTime_;
			std::cout 
				<< "\r  [View " << activeView_ << ", "
				<< "Shadertime: " << std::fixed << std::setprecision(3) << shaderTime_.count() << "ms, "
				<< "Frametime: " << std::fixed << std::setprecision(3) << frameTime_.count() << "ms, "
				<< std::fixed << std::setprecision(3) << (frameCount_/sec.count()) << "avgFPS, "
				<< (v->currentPrimitive_ == GL_TRIANGLES ? v->currentVertexCount_/3 : v->currentVertexCount_)
				<< (v->currentPrimitive_ == GL_TRIANGLES ? " Tris]" : "Points]") << std::flush;
		}

		// poll standard input before reading so that it is non-blocking
		// this will not work for other shells than Microsoft
		if (WaitForSingleObject(GetStdHandle(STD_INPUT_HANDLE), pollInterval) == WAIT_OBJECT_0) {
			INPUT_RECORD buf[128]; DWORD readCount;
			if (ReadConsoleInput(GetStdHandle(STD_INPUT_HANDLE), buf, 128, &readCount)) {
				for (DWORD i = 0; i < readCount; i++)  {
		            if (buf[i].EventType == KEY_EVENT) {
		            	if (buf[i].Event.KeyEvent.bKeyDown) {

		                    char c = buf[i].Event.KeyEvent.uChar.AsciiChar;
		                    if (c == '\r' || c == '\n') { // enter
		                    	if (in.size() > 0) {
									v->fragmentShaderSourceTmp_ = v->fragmentShaderSource_ + "  " + in + '\n';
									inputThreadFlag_ = true;
									in = "";
								}
								std::cout << '\n';
		                    } else if (c == 8) { // backspace
		                    	if (in.size() > 0) {
		                    		std::string whitespace(in.size(), ' ');
		                    		std::cout << '\r' << "  " << whitespace << std::flush;
		                    		in.resize(in.size() - 1);
		                    		std::cout << '\r' << "  " << in << std::flush;
		                    	}
		                    } else if (c >= 32 && c <= 126) { // printable
		                    	if (in.size() == 0) std::cout << "\n  " << std::flush;
		                    	in += c;
		                    	std::cout << c;
		                    }
		                    if (buf[i].Event.KeyEvent.wVirtualKeyCode == 27) { // escape
		                    	isRunning_ = false;
		                    	break;
		                    }

							WORD vk = buf[i].Event.KeyEvent.wVirtualKeyCode;
							if (vk == VK_PRIOR) {
								activeView_ = (activeView_ + 1) % viewStates_.size();
							}
							else if (vk == VK_NEXT) {
								activeView_ = (activeView_ == 0 ? viewStates_.size() - 1 : activeView_ - 1) % viewStates_.size();
							}
		                }
		            }
		        }
			}
		}
	}
	std::cout << std::endl << "}";
}


// public:

/**
*
*/
void createGLContexts(void* outDeviceContext, void* outRenderContext) {
	createWindowsWindow("Shader Output", width_, height_, &windowHandle_);
	HDC gdiDeviceContext = GetDC(windowHandle_);
	createGLContext(gdiDeviceContext, &glRenderContext_);
	if (outDeviceContext && outRenderContext) {
		*((HDC*)outDeviceContext) = gdiDeviceContext;
		*((HGLRC*)outRenderContext) = glRenderContext_;
	}

	loadGLFunctions();

	createGLQuad();

	initialized_ = true;
}

/**
*
*/
void createGLQuad() {
	float quad[] = {
		// (x, y)
		-1.0f,  1.0f, // top left
		1.0f, -1.0f, // bottom right
		-1.0f, -1.0f, // bottom left
		-1.0f,  1.0f, // top left
		1.0f,  1.0f, // top right
		1.0f, -1.0f // bottom right
	};
	GLuint id;
	glGenVertexArrays(1, &id);
	glBindVertexArray(id);
	GLuint buffer;
	glGenBuffers(1, &buffer);
	glBindBuffer(GL_ARRAY_BUFFER, buffer);
	glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
	glEnableVertexAttribArray(0);

	ViewState* v = &viewStates_[activeView_];
	v->vao_ = id;
	v->currentVertexCount_ = 6;
}

/**
*
*/
void createGLTriangles2D(size_t bytes, void* outBuffer, void* data) {
	assert(initialized_);

	ViewState* v = &viewStates_[activeView_];

	v->fragmentShaderSource_ += "  color = vec4(1);\n";

	GLuint vao = 0;
	glGenVertexArrays(1, &vao);
	glBindVertexArray(vao);

	GLuint vbo = 0; 
	glGenBuffers(1, &vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, bytes, data, GL_STATIC_DRAW);

	// Assume every vertex is 2 floats and no extra data
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
	glEnableVertexAttribArray(0);

	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindVertexArray(0);

	v->currentVertexCount_ = (GLsizei)bytes / (2*sizeof(float));

	v->currentPrimitive_ = GL_TRIANGLES;

	*((GLuint*)outBuffer) = vao;
}

/**
*
*/
void createGLPoints2D(size_t bytes, GLVertexHandle* outHandle, void* data, int stride) {
	assert(initialized_);

	ViewState* v = &viewStates_[activeView_];

	// Fake sphere normal and diffuse lighting
	v->fragmentShaderSource_ += "  vec3 normal = vec3(0, 0, 0);\n";
	v->fragmentShaderSource_ += "  normal.xy = gl_PointCoord * 2.0 - vec2(1.0);\n";
	v->fragmentShaderSource_ += "  float mag = dot(normal.xy, normal.xy);\n";
	v->fragmentShaderSource_ += "  if (mag > 1.0) discard; // kill pixels outside circle\n";
	v->fragmentShaderSource_ += "  normal.z = sqrt(1.0 - mag);\n";
	v->fragmentShaderSource_ += "  color = vec4( vec3( dot(normalize(normal), vec3(0,0,1)) ), 1.0 );\n";
	v->fragmentShaderSource_ += "  gl_FragDepth = 1.0-normal.z;\n";

	GLuint vao = 0;
	glGenVertexArrays(1, &vao);
	glBindVertexArray(vao);

	GLuint vbo = 0;
	glGenBuffers(1, &vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, bytes, data, GL_STATIC_DRAW);

	// Assume every vertex is 2 floats and no extra data
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, 0);
	glEnableVertexAttribArray(0);

	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindVertexArray(0);

	if (stride) v->currentVertexCount_ = (GLsizei)bytes / stride;
	else v->currentVertexCount_ = (GLsizei)bytes / (2 * sizeof(float));

	v->currentPrimitive_ = GL_POINTS;
	glPointSize(20.0f);

	v->vao_ = vao;
	*outHandle = { vbo, vao };
}

/**
*
*/
void updateGLVertexData(GLVertexHandle handle, size_t bytes, void* data) {
	glBindBuffer(GL_ARRAY_BUFFER, handle.vbo);
	glBufferData(GL_ARRAY_BUFFER, bytes, data, GL_DYNAMIC_DRAW);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
}

/**
* Create a uniform buffer, connect to shader and return GL handle.
*
* Use explicit binding points for uniform buffers
* https://www.khronos.org/opengl/wiki/Layout_Qualifier_(GLSL)#Binding_points
*
* Explicit locations are used for images (i.e. samplers),
* and we HOPE there are no conflicts (TESTS NEEDED!).
*/
void createGLBuffer(size_t bytes, void* outBuffer) {
	assert(initialized_);

	ViewState* v = &viewStates_[activeView_];

	GLint maxSize; glGetIntegerv(GL_MAX_UNIFORM_BLOCK_SIZE, &maxSize);
	if (bytes > (size_t)maxSize) {
		std::cout << bytes << " exceeds max size of " << maxSize << " for GL uniform buffer" << std::endl;
		return;
	}
	static GLuint bindingPoint = 0; // assignemt happens once at program start, then value persists

	std::string bindingPointString = std::to_string(bindingPoint);
	std::string uniformName = "Buf" + bindingPointString;
	size_t words = bytes / 4;

	// to access each scalar in array, use std140 memory layout rules and access through vec4
	// https://www.khronos.org/opengl/wiki/Interface_Block_(GLSL)#Memory_layout
	v->glslUniformString_ += "layout(std140, binding=" + bindingPointString + ") uniform " + uniformName
		+ " {\n  uvec4 buf" + bindingPointString + "[" + std::to_string(words/4) + "];"
		+ " // " + std::to_string(words) + " uints, " + std::to_string(bytes) + " bytes"
		+ "\n};\n";
	v->fragmentShaderSource_ = v->fragmentShaderSource_
		+ "  i = int(floor(p.x * " + std::to_string(words) + "));\n"
		+ "  f = float(buf" + bindingPointString + "[i/4][i%4]) / 4294967296.;\n"
		+ "  if(p.y < f) color=vec4(1); else color=vec4(0);\n";

	compileGLShader(vertexShaderSource_,
		glslVersionString_ + v->glslUniformString_ + v->fragmentShaderSource_ + "}",
		&v->vertexShader_, &v->fragmentShader_);
	v->shaderProgram_ = glCreateProgram();
	glAttachShader(v->shaderProgram_, v->fragmentShader_);
	glAttachShader(v->shaderProgram_, v->vertexShader_);
	glLinkProgram(v->shaderProgram_);

	GLuint ubo = 0;
	glGenBuffers(1, &ubo);
	glBindBuffer(GL_UNIFORM_BUFFER, ubo);
	glBufferData(GL_UNIFORM_BUFFER, bytes, NULL, GL_DYNAMIC_DRAW);
	glBindBuffer(GL_UNIFORM_BUFFER, 0);
	GLuint index = glGetUniformBlockIndex(v->shaderProgram_, uniformName.c_str());
	if (index != GL_INVALID_INDEX) {
		glUniformBlockBinding(v->shaderProgram_, index, bindingPoint);
		glBindBufferBase(GL_UNIFORM_BUFFER, bindingPoint, ubo);
	}
	bindingPoint++;
	*((GLuint*)outBuffer) = ubo;
	//TODO buffer cleanup at end of render loop
}

/**
* Create image, connect to sampler on shader side and return GL handle.
* Optionally image data can be passed which is interpreted in 32Bit greyscale.
*
* Use explicit location for samplers (set to imageCount_).
* Use one target (GL_TEXTURE_2D) for one unit (GL_TEXTURE0 + imageCount_).
* More info by Nicol Bolas: https://stackoverflow.com/a/8887844/4246148
*/
void createGLImage(int w, int h, void* outImageHandle, void* data, int channels) {
	assert(initialized_);

	ViewState* v = &viewStates_[activeView_];

	GLint maxUnits; glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &maxUnits);
	if (imageCount_ >= (unsigned int)maxUnits) {
		std::cout << "Only " << maxUnits << " texture units are guaranteed by GL" << std::endl;
		return;
	}

	const std::string countStr = std::to_string(imageCount_);
	const std::string name = "img" + countStr;
	v->glslUniformString_ += "layout(location = " + countStr + ") uniform sampler2D " + name + ";\n";
	v->fragmentShaderSource_ += "  color = texture(" + name + ", (p+1.)*.5);\n";

	GLuint tex;
	GL(GenTextures, 1, &tex);
	// When a texture is first bound it is assigned to the active texture unit
	GL(ActiveTexture, GL_TEXTURE0 + imageCount_);
	// GL_TEXTURE_2D is the target, of which a unit can have multiple, and that correspond to shader samplers
	GL(BindTexture, GL_TEXTURE_2D, tex);

	switch (channels) {
	case 3:
		GL(TexStorage2D, GL_TEXTURE_2D, 1/*#mips*/, GL_RGB8, w, h); // allocation
		if (data)
			GL(TexSubImage2D, GL_TEXTURE_2D, 0/*mip*/, 0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, (GLvoid*)data);
		break;
	default:
		GL(TexStorage2D, GL_TEXTURE_2D, 1/*#mips*/, GL_R32F, w, h); // allocation
		if (data)
			GL(TexSubImage2D, GL_TEXTURE_2D, 0/*mip*/, 0, 0, w, h, GL_RED, GL_FLOAT, (GLvoid*)data);
	}

	GL(TexParameteri, GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	GL(TexParameteri, GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	GL(TexParameteri, GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	GL(TexParameteri, GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
	GL(TexParameteri, GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
	
	if (outImageHandle)
		*((GLuint*)outImageHandle) = tex;

	if (v->imageOffset_ < 0) v->imageOffset_ = imageCount_;
	imageCount_++; v->imageCount_++;
}

/**
*
*/
void pushGLView(float scaleX, float scaleY, float translationX, float translationY) {

	activeView_ = viewStates_.size();
	ViewState view;
	view.vertexScale_[0] = scaleX, view.vertexScale_[1] = scaleY;
	view.vertexTranslation_[0] = translationX, view.vertexTranslation_[1] = translationY;
	viewStates_.push_back(view);

	// Create framebuffer for lower view so that its textures can be accessed by new view
	createGLFramebuffer(&viewStates_[activeView_-1].framebuffer_);
}

/**
*
*/
static void runGLShader_internal(ViewState* v) {

	// Vertex array
	GL(BindVertexArray, v->vao_);

	// Triangle operations
	GL(FrontFace, GL_CCW);
	GL(CullFace, GL_BACK);
	GL(PolygonMode, GL_FRONT_AND_BACK, GL_FILL);

	// Shader
	hotreloadGLShader();
	GL(UseProgram, v->shaderProgram_);

	// Vertex transform
	GL(Uniform2f, 42, v->vertexScale_[0], v->vertexScale_[1]);
	GL(Uniform2f, 43, v->vertexTranslation_[0], v->vertexTranslation_[1]);

	// Textures
	for (int i = v->imageOffset_; i < v->imageOffset_ + v->imageCount_; i++) {
		// Select all targets (2D,3D,etc.) under unit i.
		GL(ActiveTexture, GL_TEXTURE0 + i);
		// Select target that corresponds to sampler type
		GL(Uniform1i, i, i);
		// Note: We use one uniform location for one unit,
		// so that we have one sampler type for one unit.
		// It is possible to bind textures to different targets in one unit,
		// but GL forbids to render with this state.
	}

	// Viewport
	glViewport(0, 0, width_, height_);

	// Framebuffer
	GL(Clear, GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	// Pixel operations
	GL(Enable, GL_DEPTH_TEST);
	GL(DepthFunc, GL_LESS);
	GL(Enable, GL_BLEND);
	// Incoming colors are scaled with their opacity (alpha) and added 
	// to framebuffer colors that are scaled with the transparency (1-alpha)
	GL(BlendFunc, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	high_resolution_clock::time_point start = high_resolution_clock::now();

	GL(DrawArrays, v->currentPrimitive_, 0, v->currentVertexCount_);

	shaderTime_ = high_resolution_clock::now() - start;
}

/**
*
*/
void runGLShader() {
	// First write lower view into offscreen framebuffer
	if (activeView_ > 0) {
		GL(BindFramebuffer, GL_FRAMEBUFFER, viewStates_[activeView_-1].framebuffer_);
		runGLShader_internal(&viewStates_[activeView_-1]);
	}
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	runGLShader_internal(&viewStates_[activeView_]);
}

/**
* Windows message pump.
* Polling for window events and return false if window closed.
* https://en.wikipedia.org/wiki/Message_loop_in_Microsoft_Windows
*/
bool processWindowsMessage(unsigned int* mouse, bool* mouseDown, char* pressedKey) {
	MSG msg;

	if (!isRunning_) PostQuitMessage(0);

	// Query Windows for messages
	// Use PeekMessage because unlike GetMessage it does not block.
	// This enables us to control our update frequency elsewhere.
	if (PeekMessage(&msg, NULL, 0, 0, PM_NOREMOVE) != 0) {
		BOOL bRet;
		bRet = GetMessage(&msg, NULL, 0, 0);
		if (bRet > 0)  // (bRet > 0 indicates a message that must be processed.)
		{
			TranslateMessage(&msg); // Translates virtual-key messages into character messages. 
				// The character messages are posted to the calling thread's message queue, 
				// to be read the next time the thread calls the GetMessage or PeekMessage function.
			DispatchMessage(&msg); // Dispatches a message to a window procedure
		} else if (bRet < 0)  // (bRet == -1 indicates an error.)
		{
			// Handle or log the error; possibly exit.
			DWORD error = GetLastError();
			std::cout << "WINDOWS ERROR " << error << std::endl;
		} else  // (bRet == 0 indicates "exit program".)
		{
			return false;
		}
	}

	if (mouse) {
		mouse[0] = mouse_[0];
		mouse[1] = mouse_[1];
	}

	if (mouseDown) {
		*mouseDown = mouseDown_;
	}

	if (pressedKey) {
		*pressedKey = pressedKey_;
	}

	return true;
}

/**
*
*/
void openGLWindowAndREPL() {
	assert(initialized_);

	ShowWindow(windowHandle_, SW_SHOWNORMAL);
	UpdateWindow(windowHandle_);

	for (int i = 0; i < viewStates_.size(); i++) {
		ViewState* v = &viewStates_[i];
		compileGLShader(vertexShaderSource_,
			glslVersionString_ + v->glslUniformString_ + v->fragmentShaderSource_ + "}",
			&v->vertexShader_, &v->fragmentShader_);
		v->shaderProgram_ = glCreateProgram();
		glAttachShader(v->shaderProgram_, v->fragmentShader_);
		glAttachShader(v->shaderProgram_, v->vertexShader_);
		glLinkProgram(v->shaderProgram_);
	}

	launchTime_ = std::chrono::high_resolution_clock::now();

	isRunning_ = true;
}

/**
*
*/
void swapGLBuffers(double frequencyHz) {
	const double budgetMillis = 1000.0 / frequencyHz;

	// Dont show the image yet, restrict to budget by sleeping
	//TODO not accurate, try:
	// https://www.khronos.org/opengl/wiki/Swap_Interval#In_Windows
	frameTime_ = high_resolution_clock::now() - lastSwapTime_;
	double remainder = budgetMillis - frameTime_.count();
	if (remainder > 0) {
		timeBeginPeriod(1);
		Sleep((DWORD)(remainder));
		timeEndPeriod(1);
	}

	// Show the image now
	SwapBuffers(GetDC(windowHandle_));
	lastSwapTime_ = high_resolution_clock::now();
	frameCount_++;
}

/**
*
*/
void closeGLWindowAndREPL() {
	// window closed now, wait for input thread before cleanup
	isRunning_ = false;
	inputThread.join();

	ViewState* v = &viewStates_[activeView_];

	glDeleteShader(v->fragmentShader_);
	glDeleteShader(v->vertexShader_);
	glDeleteProgram(v->shaderProgram_);

	glDeleteVertexArrays(1, &v->vao_);

	// make the rendering context not current before deleting it
	wglMakeCurrent(NULL, NULL);
	wglDeleteContext(glRenderContext_);
}

/**
*
*/
void getGLWindowSize(unsigned int* s) {
	s[0] = width_;
	s[1] = height_;
}



// https://msdn.microsoft.com/de-de/library/windows/desktop/ms633504(v=vs.85).aspx
// GetDesktopWindow 
// Retrieves a handle to the desktop window. The desktop window covers the entire screen. 
// The desktop window is the area on top of which other windows are painted.