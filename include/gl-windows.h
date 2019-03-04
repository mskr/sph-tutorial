/**
*
*/
struct GLVertexHandle {
	unsigned int vbo, vao;
};

/**
*
*/
void createGLContexts(void* outDeviceContext = 0, void* outRenderContext = 0);

/**
*
*/
void createGLQuad();

/**
*
*/
void createGLTriangles2D(size_t bytes, void* outBuffer, void* data = 0);

/**
*
*/
void createGLPoints2D(size_t bytes, GLVertexHandle* outHandle, void* data = 0, int stride = 0);

/**
*
*/
void updateGLVertexData(GLVertexHandle handle, size_t bytes, void* data);

/**
*
*/
void createGLImage(int w, int h, void* outImageHandle = 0, void* data = 0, int channels = 1);

/**
*
*/
void openGLWindowAndREPL();

/**
*
*/
bool processWindowsMessage(unsigned int* mouse = 0, bool* mouseDown = 0, char* pressedKey = 0);

/**
* Acts as ImGui float slider and shader uniform
*/
struct GLShaderParam {
	const char* name = 0;
	float* ptr = 0;
	float minVal = .0f, maxVal = 1.f;
};

/**
*
*/
void runGLShader(GLShaderParam slot1 = GLShaderParam(), GLShaderParam slot2 = GLShaderParam(), GLShaderParam slot3 = GLShaderParam());

/**
*
*/
void swapGLBuffers(double hz);

/**
*
*/
void closeGLWindowAndREPL();

/**
*
*/
void getGLWindowSize(unsigned int* s);

/**
*
*/
void pushGLView(float* proj = 0);