#version 430
layout(location=42) uniform mat4 PROJ = mat4(1);
layout(location=43) uniform vec2 PX;
layout(location=44) uniform float POINT_SIZE;
layout(location=45) uniform float DELTA_T = 0.005;
layout(location = 3) uniform sampler2D COLORMAP;
layout(location = 4) uniform sampler2D DEPTHMAP;
in vec2 p;
out vec4 color;
int i; float f;
void main() {
  
  vec2 uv = (p+1.)*.5;
  vec2 pxsize = PX*.5; // px size to uv coords
  
  // Nborhood positions in NDC[-1,+1] with depth
  vec3 pleft =   vec3(p-vec2(PX.x,0), texture(DEPTHMAP, uv-vec2(pxsize.x,0)));
  vec3 pright =  vec3(p+vec2(PX.x,0), texture(DEPTHMAP, uv+vec2(pxsize.x, 0)));
  vec3 ptop =    vec3(p+vec2(0,PX.y), texture(DEPTHMAP, uv+vec2(0, pxsize.y)));
  vec3 pbottom = vec3(p-vec2(0,PX.y), texture(DEPTHMAP, uv-vec2(0,pxsize.y)));
  vec3 pcenter = vec3(p, texture(DEPTHMAP, uv));
  
  // Nborhood normal estimates by finite diffs
  vec3 nleft =   normalize(cross(pcenter-pleft, ptop-pcenter));
  vec3 nright =  normalize(cross(pright-pcenter, ptop-pcenter));
  vec3 ntop =    normalize(cross(ptop-pcenter, pright-pcenter));
  vec3 nbottom = normalize(cross(pcenter-pbottom, pright-pcenter));
  
  // Curvature (normal divergence = sum of partial diffs)
  float curv = (nright.x-nleft.x)+(nbottom.y-ntop.y);
  
  color=vec4(curv,curv,curv, 1);
  gl_FragDepth = texture(DEPTHMAP, uv).r - curv*.001;
}