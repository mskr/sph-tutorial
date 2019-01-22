#include <gl-windows.h>

#include <assert.h>
#include <windows.h>

#include <glad/glad.h>
#include <glad/glad_wgl.h>

#include <vector>
#include <iostream>
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

static GLuint vao_ = 0;

// Holds GL shader handles
static GLuint vertexShader_ = 0;
static GLuint fragmentShader_ = 0;
static GLuint shaderProgram_ = 0;

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
	p = gl_Position.xy;\
}";

// Holds declarations of currently added buffers, samplers etc.
static std::string glslUniformString_ = "";

// Holds fragment shader code, that can be extended via input at runtime
static std::string fragmentShaderSource_ =
"in vec2 p;\n\
out vec4 color;\n\
int i; float f;\n\
void main() {\n\
  color = vec4(p, 0, 1);\n";

// Holds a copy of the above after input was made, gets written back when successfully compiled
static std::string fragmentShaderSourceTmp_;

// Flag through which main thread shows that input thread should end
static bool isRunning_ = false;

// Flag showing when uncompiled input is ready
static bool inputThreadFlag_ = false;

void runREPL();
static std::thread inputThread(runREPL);

// Holds number of images added through createGLImage
static int imageCount_ = 0;

// Holds number of vertices added through createGLTriangles2D
static GLsizei currentVertexCount_ = 6;

// Holds current drawing primitive
static GLenum currentPrimitive_ = GL_TRIANGLES;

//TODO:
// Create view state for each call to createGLBuffer/Image/Triangles().
// Make them switchable with left+right arrows.
// Return ids for view states.
// Provide function to merge two states (e.g. combining two buffers in shader).
struct ViewState { //TODO move to header
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
	// Holds number of images added through createGLImage
	int imageCount_ = 0;
	// Holds number of vertices added through createGLTriangles2D
	GLsizei currentVertexCount_ = 6;
	// Holds current drawing primitive
	GLenum currentPrimitive_ = GL_TRIANGLES;
};
static std::vector<ViewState> viewStates_;
static unsigned int activeView_;

using namespace std::chrono;
static duration<double, std::milli> shaderTime_;
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
		GLuint newVertexShader, newFragmentShader;
		if (compileGLShader(vertexShaderSource_,
			glslVersionString_ + glslUniformString_ + fragmentShaderSourceTmp_ + "}", 
			&newVertexShader, &newFragmentShader)) 
		{
			fragmentShaderSource_ = fragmentShaderSourceTmp_;
			glDeleteShader(fragmentShader_);
			glDeleteShader(vertexShader_);
			glDeleteProgram(shaderProgram_);
			vertexShader_ = newVertexShader;
			fragmentShader_ = newFragmentShader;
			shaderProgram_ = glCreateProgram();
			glAttachShader(shaderProgram_, fragmentShader_);
			glAttachShader(shaderProgram_, vertexShader_);
			glLinkProgram(shaderProgram_);
			std::cout << " [OK]" << std::endl;
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
static void createGLQuadVAO(GLuint* outVAO) {
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
	*outVAO = id;
}

/**
* Polls stdin for GLSL code while isRunning_ is true.
* Sets inputThreadFlag_ when a line of code is ready to be compiled.
*/
static void runREPL() {
	while (!isRunning_) {
		//do nothing, thread is not meant to start yet
	}
	std::cout << " ____________________________________________________________" << std::endl;
	std::cout << "|::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::|" << std::endl;
	std::cout << "|:::::::::::::::::: FRAGMENT SHADER EDITOR ::::::::::::::::::|" << std::endl;
	std::cout << "|::::::: works only in Microsoft shell, ESC to close ::::::::|" << std::endl;
	std::cout << std::endl;
	std::cout << glslVersionString_ + glslUniformString_ + fragmentShaderSource_;
	const int pollInterval = 200; // milliseconds
	std::cout << "  " << std::flush; // indentation
	std::string in;
	while (isRunning_) {
		if (in.size() == 0) {
			duration<double> sec = std::chrono::high_resolution_clock::now() - launchTime_;
			std::cout 
				<< "\r  [" << shaderTime_.count() << "ms, " 
				<< (frameCount_/sec.count()) << "FPS, " 
				<< (currentPrimitive_ == GL_TRIANGLES ? currentVertexCount_/3 : currentVertexCount_)
				<< (currentPrimitive_ == GL_TRIANGLES ? " Tris]" : "Points]") << std::flush;
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
									fragmentShaderSourceTmp_ = fragmentShaderSource_ + in;
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

	createGLQuadVAO(&vao_);

	initialized_ = true;
}

/**
*
*/
void createGLTriangles2D(size_t bytes, void* outBuffer, void* data) {
	assert(initialized_);

	fragmentShaderSource_ += "  color = vec4(1);\n";

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

	currentVertexCount_ = (GLsizei)bytes / (2*sizeof(float));

	currentPrimitive_ = GL_TRIANGLES;

	*((GLuint*)outBuffer) = vao;
}

/**
*
*/
void createGLPoints2D(size_t bytes, GLVertexHandle* outHandle, void* data, size_t stride) {
	assert(initialized_);

	fragmentShaderSource_ += "  color = vec4(1);\n";

	GLuint vao = 0;
	glGenVertexArrays(1, &vao);
	glBindVertexArray(vao);

	GLuint vbo = 0;
	glGenBuffers(1, &vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, bytes, data, GL_DYNAMIC_DRAW);

	// Assume every vertex is 2 floats and no extra data
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, 0);
	glEnableVertexAttribArray(0);

	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindVertexArray(0);

	if (stride) currentVertexCount_ = (GLsizei)bytes / stride;
	else currentVertexCount_ = (GLsizei)bytes / (2 * sizeof(float));

	currentPrimitive_ = GL_POINTS;

	*outHandle = { vbo, vao };
}

/**
*
*/
void updateGLPoints2D(GLVertexHandle handle, size_t bytes, void* data, size_t stride) {
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
	glslUniformString_ += "layout(std140, binding=" + bindingPointString + ") uniform " + uniformName
		+ " {\n  uvec4 buf" + bindingPointString + "[" + std::to_string(words/4) + "];"
		+ " // " + std::to_string(words) + " uints, " + std::to_string(bytes) + " bytes"
		+ "\n};\n";
	fragmentShaderSource_ = fragmentShaderSource_
		+ "  i = int(floor(p.x * " + std::to_string(words) + "));\n"
		+ "  f = float(buf" + bindingPointString + "[i/4][i%4]) / 4294967296.;\n"
		+ "  if(p.y < f) color=vec4(1); else color=vec4(0);\n";

	compileGLShader(vertexShaderSource_,
		glslVersionString_ + glslUniformString_ + fragmentShaderSource_ + "}",
		&vertexShader_, &fragmentShader_);
	shaderProgram_ = glCreateProgram();
	glAttachShader(shaderProgram_, fragmentShader_);
	glAttachShader(shaderProgram_, vertexShader_);
	glLinkProgram(shaderProgram_);

	GLuint ubo = 0;
	glGenBuffers(1, &ubo);
	glBindBuffer(GL_UNIFORM_BUFFER, ubo);
	glBufferData(GL_UNIFORM_BUFFER, bytes, NULL, GL_DYNAMIC_DRAW);
	glBindBuffer(GL_UNIFORM_BUFFER, 0);
	GLuint index = glGetUniformBlockIndex(shaderProgram_, uniformName.c_str());
	if (index != GL_INVALID_INDEX) {
		glUniformBlockBinding(shaderProgram_, index, bindingPoint);
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
void createGLImage(int w, int h, void* outImageHandle, void* data) {
	assert(initialized_);
	GLint maxUnits; glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &maxUnits);
	if (imageCount_ >= maxUnits) {
		std::cout << "Only " << maxUnits << " texture units are guaranteed by GL" << std::endl;
		return;
	}

	const std::string countStr = std::to_string(imageCount_);
	const std::string name = "img" + countStr;
	glslUniformString_ += "layout(location = " + countStr + ") uniform sampler2D " + name + ";\n";
	fragmentShaderSource_ += "  color = texture(img" + countStr + ", p);\n";

	GLuint tex;
	GL(GenTextures, 1, &tex);
	GL(ActiveTexture, GL_TEXTURE0 + imageCount_);
	GL(BindTexture, GL_TEXTURE_2D, tex);

	// Use GL_R32_F because only greyscale needed
	GL(TexStorage2D, GL_TEXTURE_2D, 1/*#mips*/, GL_R32F, w, h); // allocation
	if (data)
		GL(TexSubImage2D, GL_TEXTURE_2D, 0/*mip*/, 0,0, w, h, GL_RED, GL_FLOAT, (GLvoid*)data);

	GL(TexParameteri, GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	GL(TexParameteri, GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	GL(TexParameteri, GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	GL(TexParameteri, GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	GL(TexParameteri, GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
	
	*((GLuint*)outImageHandle) = tex;

	imageCount_++;
}

/**
*
*/
void runGLShader(GLVertexHandle handle, float scaleX, float scaleY, float translationX, float translationY) {
	if (handle.vao < 0) handle.vao = vao_;

	hotreloadGLShader();

	// Vertex array
	glBindVertexArray(handle.vao);

	// Triangle operations
	glFrontFace(GL_CCW);
	glCullFace(GL_BACK);
	glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

	// Shader
	glUseProgram(shaderProgram_);

	// Textures
	for (int i = 0; i < imageCount_; i++) {
		// Select all targets (2D,3D,etc.) under unit i.
		glActiveTexture(GL_TEXTURE0 + i);
		// Select target that corresponds to sampler type
		glUniform1i(i, i);
		// Note: We use one uniform location for one unit,
		// so that we have one sampler type for one unit.
		// It is possible to bind textures to different targets in one unit,
		// but GL forbids to render with this state.
	}

	glUniform2f(42, scaleX, scaleY);
	glUniform2f(43, translationX, translationY);

	// Viewport
	glViewport(0, 0, width_, height_);

	// Framebuffer
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glClear(GL_COLOR_BUFFER_BIT);

	// Pixel operations
	glDisable(GL_DEPTH_TEST);
	glEnable(GL_BLEND);
	// Incoming colors are scaled with their opacity (alpha) and added 
	// to framebuffer colors that are scaled with the transparency (1-alpha)
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	glPointSize(2.0f);

	high_resolution_clock::time_point start = high_resolution_clock::now();

	glDrawArrays(currentPrimitive_, 0, currentVertexCount_);

	shaderTime_ = high_resolution_clock::now() - start;
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

	compileGLShader(vertexShaderSource_,
		glslVersionString_ + glslUniformString_ + fragmentShaderSource_ + "}",
		&vertexShader_, &fragmentShader_);
	shaderProgram_ = glCreateProgram();
	glAttachShader(shaderProgram_, fragmentShader_);
	glAttachShader(shaderProgram_, vertexShader_);
	glLinkProgram(shaderProgram_);

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
	duration<double, std::milli> frameTime = high_resolution_clock::now() - lastSwapTime_;
	double remainder = budgetMillis - frameTime.count();
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

	glDeleteShader(fragmentShader_);
	glDeleteShader(vertexShader_);
	glDeleteProgram(shaderProgram_);

	glDeleteVertexArrays(1, &vao_);

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