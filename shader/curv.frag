#version 430
layout(location=42) uniform mat4 PROJ = mat4(1);
layout(location=43) uniform vec2 PX_SIZE;
layout(location=44) uniform float POINT_SIZE;
layout(location=45) uniform float DELTA_T = 0.005;
layout(location = 3) uniform sampler2D COLORMAP;
layout(location = 4) uniform sampler2D DEPTHMAP;
in vec3 p;
out vec4 color;
int i; float f;
void main() {
  
  const vec2 uv = (p.xy+1.)*.5;

  const vec2 UV_PX_SIZE = PX_SIZE*.5;
  const vec2 DX = vec2(UV_PX_SIZE.x, 0.0);
  const vec2 DY = vec2(0.0, UV_PX_SIZE.y);

    float zc = texture(DEPTHMAP, uv).r - 1.0;
    float zdxp = (texture(DEPTHMAP, uv + DX).r - 1.0);
    float zdxn = (texture(DEPTHMAP, uv - DX).r - 1.0);
    float zdyp = (texture(DEPTHMAP, uv + DY).r - 1.0);
    float zdyn = (texture(DEPTHMAP, uv - DY).r - 1.0);

    // Finite diff of z in x direction
    float zdx = 0.5 * (zdxp - zdxn);

    // Finite diff of z in y direction
    float zdy = 0.5 * (zdyp - zdyn);

    // Second order finite diffs
    float zdx2 = zdxp + zdxn - 2.0 * zc;
    float zdy2 = zdyp + zdyn - 2.0 * zc;

    // Second order finite differences, alternating variables
    float zdxpyp = texture(DEPTHMAP, uv + DX + DY).r - 1.0;
    float zdxnyn = texture(DEPTHMAP, uv - DX - DY).r - 1.0;
    float zdxpyn = texture(DEPTHMAP, uv + DX - DY).r - 1.0;
    float zdxnyp = texture(DEPTHMAP, uv - DX + DY).r - 1.0;
    
    float zdxy = (zdxpyp + zdxnyn - zdxpyn - zdxnyp) / 4.0;

    // Boundary conditions
    if(abs(zdx) > POINT_SIZE * 5.0) {
        zdx  = 0.0;
        zdx2 = 0.0;
    }

    if(abs(zdy) > POINT_SIZE * 5.0) {
        zdy  = 0.0;
        zdy2 = 0.0;
    }

    if(abs(zdxy) > POINT_SIZE * 5.0) {
        zdxy = 0.0;
    }

    // Projection transform inversion terms
    float cx = 2.0f / (1.0/DX.x * -PROJ[0][0]);
    float cy = 2.0f / (1.0/DY.y * -PROJ[1][1]);
    
    // Normalization term
    float d = cy * cy * zdx * zdx + cx * cx * zdy * zdy + cx * cx * cy * cy * zc * zc;

    // Derivatives of said term
    float ddx = cy * cy * 2.0f * zdx * zdx2 + cx * cx * 2.0f * zdy * zdxy + cx * cx * cy * cy * 2.0f * zc * zdx;
    float ddy = cy * cy * 2.0f * zdx * zdxy + cx * cx * 2.0f * zdy * zdy2 + cx * cx * cy * cy * 2.0f * zc * zdy;

    // Temporary variables to calculate mean curvature
    float ex = 0.5f * zdx * ddx - zdx2 * d;
    float ey = 0.5f * zdy * ddy - zdy2 * d;

    // Finally, mean curvature
    float h = 0.5f * ((cy * ex + cx * ey) / pow(d, 1.5f));

  
  vec3 dxyz = vec3(zdx, zdy, h);

  const float dt   = 0.0003f;
  const float dzt  = 1000.0f;



  color=vec4(dxyz, 1);
  gl_FragDepth = texture(DEPTHMAP, uv).r + dxyz.z * dt * (1.0f + (abs(dxyz.x) + abs(dxyz.y)) * dzt);
}