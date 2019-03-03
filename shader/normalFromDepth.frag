#version 430
layout(location=42) uniform mat4 PROJ = mat4(1);
layout(location=43) uniform vec2 PX;
layout(location=44) uniform float POINT_SIZE;
layout(location = 3) uniform sampler2D COLORMAP;
layout(location = 4) uniform sampler2D DEPTHMAP;
in vec2 p;
out vec4 color;
int i; float f;
void main() {

  float z0 = texture(DEPTHMAP, (p+1.)*.5).r;
  float z1x = texture(DEPTHMAP, (p+1.)*.5 + vec2(PX.x*.5, 0)).r;
  float z1y = texture(DEPTHMAP, (p+1.)*.5 + vec2(0, PX.y*.5)).r;

  vec3 P0 = vec3(p, z0);
  vec3 P1x = vec3(p+vec2(PX.x,0), z1x);
  vec3 P1y = vec3(p+vec2(0,PX.y), z1y);

  vec3 n0 = normalize(cross(P1x - P0, P1y - P0));
  color=vec4(-n0.xy, n0.z, 1);
}