vec3 v_worldPosition : TEXCOORD0 = vec3(0.0, 0.0, 0.0);
vec3 v_worldNormal   : TEXCOORD1 = vec3(0.0, 0.0, 1.0);
vec3 v_albedo         : TEXCOORD2 = vec3(1.0, 1.0, 1.0);
vec2 v_texcoord0      : TEXCOORD3 = vec2(0.0, 0.0);
vec2 v_material       : TEXCOORD4 = vec2(1.0, 0.0);
vec4 v_worldTangent   : TEXCOORD5 = vec4(0.0, 0.0, 0.0, 0.0);

vec3 a_position : POSITION;
vec3 a_normal   : NORMAL0;
vec3 a_color0   : COLOR0;
vec2 a_texcoord0 : TEXCOORD0;
vec2 a_texcoord1 : TEXCOORD1;
vec4 a_tangent   : TANGENT;
