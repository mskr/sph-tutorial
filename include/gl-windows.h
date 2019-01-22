/**
*
*/
struct GLVertexHandle {
	size_t vbo, vao;
};

/**
*
*/
void createGLContexts(void* outDeviceContext = 0, void* outRenderContext = 0);

/**
*
*/
void createGLTriangles2D(size_t bytes, void* outBuffer, void* data = 0);

/**
*
*/
void createGLPoints2D(size_t bytes, GLVertexHandle* outHandle, void* data = 0, size_t stride = 0);

/**
*
*/
void updateGLPoints2D(GLVertexHandle handle, size_t bytes, void* data, size_t stride = 0);

/**
*
*/
void createGLImage(int w, int h, void* outImageHandle, void* data = 0);

/**
*
*/
void openGLWindowAndREPL();

/**
*
*/
bool processWindowsMessage(unsigned int* mouse = 0, bool* mouseDown = 0, char* pressedKey = 0);

/**
*
*/
void runGLShader(GLVertexHandle handle, float scaleX=1, float scaleY=1, float translationX=0, float translationY=0);

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