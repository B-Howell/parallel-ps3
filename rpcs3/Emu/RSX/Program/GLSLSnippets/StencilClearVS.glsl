R"(
#version 450
#extension GL_ARB_separate_shader_objects : enable

void main()
{
	vec2 positions[] = {vec2(-1., -1.), vec2(1., -1.), vec2(-1., 1.), vec2(1., 1.)};
	gl_Position = vec4(positions[gl_VertexIndex % 4], 0., 1.);
}
)"
