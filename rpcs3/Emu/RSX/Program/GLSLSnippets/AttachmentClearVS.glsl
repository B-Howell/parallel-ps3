R"(
#version 450
#extension GL_ARB_separate_shader_objects : enable
layout(push_constant) uniform static_data{ vec4 regs[2]; };
layout(location=0) out vec4 color;

void main()
{
	vec2 positions[] = {vec2(-1., -1.), vec2(1., -1.), vec2(-1., 1.), vec2(1., 1.)};
	color = regs[0];
	gl_Position = vec4(positions[gl_VertexIndex % 4], 0., 1.);
}
)"
