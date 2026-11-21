#pragma once
#include "../Program/VertexProgramDecompiler.h"
#include "glutils/program.h"

#include <unordered_map>

enum
{
	GL_VP_FORCE_ATTRIB_SCALING = 1,	//Scale vertex read result
	GL_VP_ATTRIB_S16_INT = (1 << 1),	//Attrib is a signed 16-bit integer
	GL_VP_ATTRIB_S32_INT = (1 << 2),	//Attrib is a signed 32-bit integer

	GL_VP_SINT_MASK = (GL_VP_ATTRIB_S16_INT|GL_VP_ATTRIB_S32_INT)
};

namespace gl
{
	class shader_interpreter;
};

struct GLVertexDecompilerThread : public VertexProgramDecompiler
{
	friend class gl::shader_interpreter;

	std::string &m_shader;
protected:
	std::string getFloatTypeName(usz elementCount) override;
	std::string getIntTypeName(usz elementCount) override;
	std::string getFunction(FUNCTION f) override;
	std::string compareFunction(COMPARE f, std::string_view Op0, std::string_view Op1, bool scalar) override;

	void insertHeader(fmt::text_writer& OS) override;
	void insertInputs(fmt::text_writer& OS, const std::vector<ParamType>& inputs) override;
	void insertConstants(fmt::text_writer& OS, const std::vector<ParamType>& constants) override;
	void insertOutputs(fmt::text_writer& OS, const std::vector<ParamType>& outputs) override;
	void insertMainStart(fmt::text_writer& OS) override;
	void insertMainEnd(fmt::text_writer& OS) override;

	const RSXVertexProgram &rsx_vertex_program;
	std::unordered_map<std::string, int> input_locations;
public:
	GLVertexDecompilerThread(const RSXVertexProgram& prog, std::string& shader, ParamArray&)
		: VertexProgramDecompiler(prog)
		, m_shader(shader)
		, rsx_vertex_program(prog)
	{
	}

	void Task();
};

class GLVertexProgram : public rsx::VertexProgramBase
{
public:
	GLVertexProgram();
	~GLVertexProgram();

	ParamArray parr;
	gl::glsl::shader shader;

	void Decompile(const RSXVertexProgram& prog);

private:
	void Delete();
};
