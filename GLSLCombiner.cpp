#include <assert.h>
#include <stdio.h>
#include "N64.h"
#include "OpenGL.h"
#include "Config.h"
#include "Combiner.h"
#include "GLSLCombiner.h"
#include "Shaders.h"
#include "Noise_shader.h"
#include "FrameBuffer.h"
#include "DepthBuffer.h"

static GLuint  g_vertex_shader_object;
static GLuint  g_calc_light_shader_object;
static GLuint  g_calc_lod_shader_object;
static GLuint  g_calc_noise_shader_object;
static GLuint  g_calc_depth_shader_object;
static GLuint  g_test_alpha_shader_object;
static GLuint g_zlut_tex = 0;

static GLuint  g_shadow_map_vertex_shader_object;
static GLuint  g_shadow_map_fragment_shader_object;
static GLuint  g_draw_shadow_map_program;
GLuint g_tlut_tex = 0;
static u32 g_paletteCRC256 = 0;

static
void display_warning(const char *text, ...)
{
	static int first_message = 100;
	if (first_message)
	{
		char buf[1000];

		va_list ap;

		va_start(ap, text);
		vsprintf(buf, text, ap);
		va_end(ap);
		first_message--;
	}
}

static const GLsizei nShaderLogSize = 1024;
static
bool check_shader_compile_status(GLuint obj)
{
	GLint status;
	glGetShaderiv(obj, GL_COMPILE_STATUS, &status);
	if(status == GL_FALSE)
	{
		GLchar shader_log[nShaderLogSize];
		GLsizei nLogSize = nShaderLogSize;
		glGetShaderInfoLog(obj, nShaderLogSize, &nLogSize, shader_log);
		shader_log[nLogSize] = 0;
		display_warning(shader_log);
		return false;
	}
	return true;
}

static
bool check_program_link_status(GLuint obj)
{
	GLint status;
	glGetProgramiv(obj, GL_LINK_STATUS, &status);
	if(status == GL_FALSE)
	{
		GLsizei nLogSize = nShaderLogSize;
		GLchar shader_log[nShaderLogSize];
		glGetProgramInfoLog(obj, nShaderLogSize, &nLogSize, shader_log);
		display_warning(shader_log);
		return false;
	}
	return true;
}

#ifndef GLES2
static
void InitZlutTexture()
{
	if (!OGL.bImageTexture)
		return;

	u16 * zLUT = new u16[0x40000];
	for(int i=0; i<0x40000; i++) {
		u32 exponent = 0;
		u32 testbit = 1 << 17;
		while((i & testbit) && (exponent < 7)) {
			exponent++;
			testbit = 1 << (17 - exponent);
		}

		u32 mantissa = (i >> (6 - (6 < exponent ? 6 : exponent))) & 0x7ff;
		zLUT[i] = (u16)(((exponent << 11) | mantissa) << 2);
	}
	glGenTextures(1, &g_zlut_tex);
	glBindTexture(GL_TEXTURE_2D, g_zlut_tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_R16,
		512, 512, 0, GL_RED, GL_UNSIGNED_SHORT,
		zLUT);
	delete[] zLUT;
	glBindImageTexture(ZlutImageUnit, g_zlut_tex, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16UI);
}

static
void DestroyZlutTexture()
{
	if (!OGL.bImageTexture)
		return;
	glBindImageTexture(ZlutImageUnit, 0, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16UI);
	if (g_zlut_tex > 0)
		glDeleteTextures(1, &g_zlut_tex);
}

static
void InitShadowMapShader()
{
	if (!OGL.bImageTexture)
		return;

	g_paletteCRC256 = 0;
	glGenTextures(1, &g_tlut_tex);
	glBindTexture(GL_TEXTURE_1D, g_tlut_tex);
	glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_WRAP_S, GL_CLAMP);
	glTexImage1D(GL_TEXTURE_1D, 0, GL_R16, 256, 0, GL_RED, GL_UNSIGNED_SHORT, NULL);

	g_shadow_map_vertex_shader_object = glCreateShader(GL_VERTEX_SHADER);
	glShaderSource(g_shadow_map_vertex_shader_object, 1, &shadow_map_vertex_shader, NULL);
	glCompileShader(g_shadow_map_vertex_shader_object);
	assert(check_shader_compile_status(g_shadow_map_vertex_shader_object));

	g_shadow_map_fragment_shader_object = glCreateShader(GL_FRAGMENT_SHADER);
	glShaderSource(g_shadow_map_fragment_shader_object, 1, &shadow_map_fragment_shader_float, NULL);
	glCompileShader(g_shadow_map_fragment_shader_object);
	assert(check_shader_compile_status(g_shadow_map_fragment_shader_object));

	g_draw_shadow_map_program = glCreateProgram();
	glBindAttribLocation(g_draw_shadow_map_program, SC_POSITION, "aPosition");
	glAttachShader(g_draw_shadow_map_program, g_shadow_map_vertex_shader_object);
	glAttachShader(g_draw_shadow_map_program, g_shadow_map_fragment_shader_object);
	glLinkProgram(g_draw_shadow_map_program);
	assert(check_program_link_status(g_draw_shadow_map_program));
}

static
void DestroyShadowMapShader()
{
	if (!OGL.bImageTexture)
		return;

	glBindImageTexture(TlutImageUnit, 0, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16UI);

	if (g_tlut_tex > 0)
		glDeleteTextures(1, &g_tlut_tex);
	glDetachShader(g_draw_shadow_map_program, g_shadow_map_vertex_shader_object);
	glDetachShader(g_draw_shadow_map_program, g_shadow_map_fragment_shader_object);
	glDeleteShader(g_shadow_map_vertex_shader_object);
	glDeleteShader(g_shadow_map_fragment_shader_object);
	glDeleteProgram(g_draw_shadow_map_program);
}
#endif // GLES2

void InitGLSLCombiner()
{
	glActiveTexture(GL_TEXTURE0);
	glEnable(GL_TEXTURE_2D);
	glActiveTexture(GL_TEXTURE1);
	glEnable(GL_TEXTURE_2D);

	g_vertex_shader_object = glCreateShader(GL_VERTEX_SHADER);
	glShaderSource(g_vertex_shader_object, 1, &vertex_shader, NULL);
	glCompileShader(g_vertex_shader_object);
	assert(check_shader_compile_status(g_vertex_shader_object));

	g_calc_light_shader_object = glCreateShader(GL_FRAGMENT_SHADER);
	glShaderSource(g_calc_light_shader_object, 1, &fragment_shader_calc_light, NULL);
	glCompileShader(g_calc_light_shader_object);
	assert(check_shader_compile_status(g_calc_light_shader_object));

	g_calc_lod_shader_object = glCreateShader(GL_FRAGMENT_SHADER);
	glShaderSource(g_calc_lod_shader_object, 1, &fragment_shader_calc_lod, NULL);
	glCompileShader(g_calc_lod_shader_object);
	assert(check_shader_compile_status(g_calc_lod_shader_object));

	g_calc_noise_shader_object = glCreateShader(GL_FRAGMENT_SHADER);
	glShaderSource(g_calc_noise_shader_object, 1, &noise_fragment_shader, NULL);
	glCompileShader(g_calc_noise_shader_object);
	assert(check_shader_compile_status(g_calc_noise_shader_object));

	g_test_alpha_shader_object = glCreateShader(GL_FRAGMENT_SHADER);
	glShaderSource(g_test_alpha_shader_object, 1, &alpha_test_fragment_shader, NULL);
	glCompileShader(g_test_alpha_shader_object);
	assert(check_shader_compile_status(g_test_alpha_shader_object));

	if (OGL.bImageTexture) {
		g_calc_depth_shader_object = glCreateShader(GL_FRAGMENT_SHADER);
		glShaderSource(g_calc_depth_shader_object, 1, &depth_compare_shader_float, NULL);
		glCompileShader(g_calc_depth_shader_object);
		assert(check_shader_compile_status(g_calc_depth_shader_object));
	}

#ifndef GLES2
	InitZlutTexture();
	InitShadowMapShader();
#endif
}

void DestroyGLSLCombiner() {
	glBindFramebuffer(GL_FRAMEBUFFER, 0);

#ifndef GLES2
	DestroyZlutTexture();
	DestroyShadowMapShader();
#endif
}

const char *ColorInput_1cycle[] = {
	"combined_color.rgb",
	"readtex0.rgb",
	"readtex1.rgb",
	"uPrimColor.rgb",
	"vec_color.rgb",
	"uEnvColor.rgb",
	"uCenterColor.rgb",
	"uScaleColor.rgb",
	"combined_color.a",
	"readtex0.a",
	"readtex1.a",
	"uPrimColor.a",
	"vec_color.a",
	"uEnvColor.a",
	"lod_frac", // TODO: emulate lod_fraction
	"vec3(uPrimLod)",
	"vec3(0.5 + 0.5*snoise(noiseCoord2D))",
	"vec3(uK4)",
	"vec3(uK5)",
	"vec3(1.0)",
	"vec3(0.0)"
};

const char *ColorInput_2cycle[] = {
	"combined_color.rgb",
	"readtex1.rgb",
	"readtex0.rgb",
	"uPrimColor.rgb",
	"vec_color.rgb",
	"uEnvColor.rgb",
	"uCenterColor.rgb",
	"uScaleColor.rgb",
	"combined_color.a",
	"readtex1.a",
	"readtex0.a",
	"uPrimColor.a",
	"vec_color.a",
	"uEnvColor.a",
	"lod_frac", // TODO: emulate lod_fraction
	"vec3(uPrimLod)",
	"vec3(0.5 + 0.5*snoise(noiseCoord2D))",
	"vec3(uK4)",
	"vec3(uK5)",
	"vec3(1.0)",
	"vec3(0.0)"
};

const char *AlphaInput_1cycle[] = {
	"combined_color.a",
	"readtex0.a",
	"readtex1.a",
	"uPrimColor.a",
	"vec_color.a",
	"uEnvColor.a",
	"uCenterColor.a",
	"uScaleColor.a",
	"combined_color.a",
	"readtex0.a",
	"readtex1.a",
	"uPrimColor.a",
	"vec_color.a",
	"uEnvColor.a",
	"lod_frac",
	"uPrimLod",
	"1.0",
	"uK4",
	"uK5",
	"1.0",
	"0.0"
};

const char *AlphaInput_2cycle[] = {
	"combined_color.a",
	"readtex1.a",
	"readtex0.a",
	"uPrimColor.a",
	"vec_color.a",
	"uEnvColor.a",
	"uCenterColor.a",
	"uScaleColor.a",
	"combined_color.a",
	"readtex1.a",
	"readtex0.a",
	"uPrimColor.a",
	"vec_color.a",
	"uEnvColor.a",
	"lod_frac",
	"uPrimLod",
	"1.0",
	"uK4",
	"uK5",
	"1.0",
	"0.0"
};


static
int CompileCombiner(const CombinerStage & _stage, const char** _Input, char * _fragment_shader) {
	char buf[128];
	bool bBracketOpen = false;
	int nRes = 0;
	for (int i = 0; i < _stage.numOps; ++i) {
		switch(_stage.op[i].op) {
			case LOAD:
				sprintf(buf, "(%s ", _Input[_stage.op[i].param1]);
				strcat(_fragment_shader, buf);
				bBracketOpen = true;
				nRes |= 1 << _stage.op[i].param1;
				break;
			case SUB:
				if (bBracketOpen) {
					sprintf(buf, "- %s)", _Input[_stage.op[i].param1]);
					bBracketOpen = false;
				} else
					sprintf(buf, "- %s", _Input[_stage.op[i].param1]);
				strcat(_fragment_shader, buf);
				nRes |= 1 << _stage.op[i].param1;
				break;
			case ADD:
				if (bBracketOpen) {
					sprintf(buf, "+ %s)", _Input[_stage.op[i].param1]);
					bBracketOpen = false;
				} else
					sprintf(buf, "+ %s", _Input[_stage.op[i].param1]);
				strcat(_fragment_shader, buf);
				nRes |= 1 << _stage.op[i].param1;
				break;
			case MUL:
				if (bBracketOpen) {
					sprintf(buf, ")*%s", _Input[_stage.op[i].param1]);
					bBracketOpen = false;
				} else
					sprintf(buf, "*%s", _Input[_stage.op[i].param1]);
				strcat(_fragment_shader, buf);
				nRes |= 1 << _stage.op[i].param1;
				break;
			case INTER:
				sprintf(buf, "mix(%s, %s, %s)", _Input[_stage.op[0].param2], _Input[_stage.op[0].param1], _Input[_stage.op[0].param3]);
				strcat(_fragment_shader, buf);
				nRes |= 1 << _stage.op[i].param1;
				nRes |= 1 << _stage.op[i].param2;
				nRes |= 1 << _stage.op[i].param3;
				break;

				//			default:
				//				assert(false);
		}
	}
	if (bBracketOpen)
		strcat(_fragment_shader, ")");
	strcat(_fragment_shader, "; \n");
	return nRes;
}

GLSLCombiner::GLSLCombiner(Combiner *_color, Combiner *_alpha) {
	char *fragment_shader = (char*)malloc(4096);
	strcpy(fragment_shader, fragment_shader_header_common_variables);

	char strCombiner[512];
	strcpy(strCombiner, "  alpha1 = ");
	m_nInputs = CompileCombiner(_alpha->stage[0], AlphaInput_1cycle, strCombiner);
	strcat(strCombiner, "  color1 = ");
	m_nInputs |= CompileCombiner(_color->stage[0], ColorInput_1cycle, strCombiner);
	strcat(strCombiner, "  combined_color = vec4(color1, alpha1); \n");
	if (_alpha->numStages == 2) {
		strcat(strCombiner, "  alpha2 = ");
		m_nInputs |= CompileCombiner(_alpha->stage[1], AlphaInput_2cycle, strCombiner);
	} else
		strcat(strCombiner, "  alpha2 = alpha1; \n");
	if (_color->numStages == 2) {
		strcat(strCombiner, "  color2 = ");
		m_nInputs |= CompileCombiner(_color->stage[1], ColorInput_2cycle, strCombiner);
	} else
		strcat(strCombiner, "  color2 = color1; \n");

	strcat(fragment_shader, fragment_shader_header_common_functions);
	strcat(fragment_shader, fragment_shader_header_main);
	const bool bUseLod = (m_nInputs & (1<<LOD_FRACTION)) > 0;
	if (bUseLod)
		strcat(fragment_shader, "  float lod_frac = calc_lod(uPrimLod, vLodTexCoord);	\n");
	if ((m_nInputs & ((1<<TEXEL0)|(1<<TEXEL1)|(1<<TEXEL0_ALPHA)|(1<<TEXEL1_ALPHA))) > 0) {
		strcat(fragment_shader, fragment_shader_readtex0color);
		strcat(fragment_shader, fragment_shader_readtex1color);
	} else {
		assert(strstr(strCombiner, "readtex") == 0);
	}
	if (config.enableHWLighting)
		strcat(fragment_shader, "  float intensity = calc_light(int(vNumLights), vShadeColor.rgb, input_color); \n");
	else
		strcat(fragment_shader, "  input_color = vShadeColor.rgb;\n");
	strcat(fragment_shader, "  vec_color = vec4(input_color, vShadeColor.a); \n");
	strcat(fragment_shader, strCombiner);
	strcat(fragment_shader, "  gl_FragColor = vec4(color2, alpha2); \n");

	strcat(fragment_shader, "  if (!alpha_test(gl_FragColor.a)) discard;	\n");
	if (OGL.bImageTexture) {
		if (config.frameBufferEmulation.N64DepthCompare)
			strcat(fragment_shader, "  if (!depth_compare()) discard; \n");
		else
			strcat(fragment_shader, "  depth_compare(); \n");
	}

#ifdef USE_TOONIFY
	strcat(fragment_shader, "  toonify(intensity); \n");
#endif
	strcat(fragment_shader, "  if (uEnableFog != 0) \n");
	strcat(fragment_shader, "    gl_FragColor = vec4(mix(gl_FragColor.rgb, uFogColor.rgb, gl_FogFragCoord), gl_FragColor.a); \n");

	strcat(fragment_shader, fragment_shader_end);

#ifdef USE_TOONIFY
	strcat(fragment_shader, fragment_shader_toonify);
#endif

	GLuint fragmentShader = glCreateShader(GL_FRAGMENT_SHADER);
	glShaderSource(fragmentShader, 1, (const GLchar**)&fragment_shader, NULL);
	free(fragment_shader);
	glCompileShader(fragmentShader);
	assert(check_shader_compile_status(fragmentShader));

	memset(m_aShaders, 0, sizeof(m_aShaders));
	u32 uShaderIdx = 0;

	m_program = glCreateProgram();
	_locate_attributes();
	glAttachShader(m_program, g_vertex_shader_object);
	m_aShaders[uShaderIdx++] = g_vertex_shader_object;
	glAttachShader(m_program, fragmentShader);
	m_aShaders[uShaderIdx++] = fragmentShader;
	if (config.enableHWLighting) {
		glAttachShader(m_program, g_calc_light_shader_object);
		m_aShaders[uShaderIdx++] = g_calc_light_shader_object;
	}
	if (bUseLod) {
		glAttachShader(m_program, g_calc_lod_shader_object);
		m_aShaders[uShaderIdx++] = g_calc_lod_shader_object;
	}
	glAttachShader(m_program, g_test_alpha_shader_object);
	m_aShaders[uShaderIdx++] = g_test_alpha_shader_object;
	if (OGL.bImageTexture) {
		glAttachShader(m_program, g_calc_depth_shader_object);
		m_aShaders[uShaderIdx++] = g_calc_depth_shader_object;
	}
	glAttachShader(m_program, g_calc_noise_shader_object);
	m_aShaders[uShaderIdx] = g_calc_noise_shader_object;
	assert(uShaderIdx <= sizeof(m_aShaders)/sizeof(m_aShaders[0]));
	glLinkProgram(m_program);
	assert(check_program_link_status(m_program));
	_locateUniforms();
}

GLSLCombiner::~GLSLCombiner() {
	u32 shaderIndex = 0;
	const u32 arraySize = sizeof(m_aShaders)/sizeof(m_aShaders[0]);
	while (shaderIndex < arraySize && m_aShaders[shaderIndex] > 0) {
		glDetachShader(m_program, m_aShaders[shaderIndex]);
		glDeleteShader(m_aShaders[shaderIndex++]);
	}
	glDeleteProgram(m_program);
}

#define LocateUniform(A) \
	m_uniforms.A.loc = glGetUniformLocation(m_program, #A);

void GLSLCombiner::_locateUniforms() {
	LocateUniform(uTex0);
	LocateUniform(uTex1);
	LocateUniform(uTlutImage);
	LocateUniform(uZlutImage);
	LocateUniform(uDepthImage);
	LocateUniform(uEnableFog);
	LocateUniform(uEnableDither);
	LocateUniform(uEnableLod);
	LocateUniform(uEnableAlphaTest);
	LocateUniform(uEnableDepth);
	LocateUniform(uEnableDepthCompare)
	LocateUniform(uEnableDepthUpdate);
	LocateUniform(uDepthMode);
	LocateUniform(uFb8Bit);
	LocateUniform(uFbFixedAlpha);
	LocateUniform(uMaxTile)
	LocateUniform(uTextureDetail);
	LocateUniform(uTexturePersp);

	LocateUniform(uFogMultiplier);
	LocateUniform(uFogOffset);
	LocateUniform(uK4);
	LocateUniform(uK5);
	LocateUniform(uPrimLod);
	LocateUniform(uNoiseTime);
	LocateUniform(uLodXScale);
	LocateUniform(uLodYScale);
	LocateUniform(uMinLod);
	LocateUniform(uDepthTrans);
	LocateUniform(uDepthScale);
	LocateUniform(uAlphaTestValue);

	LocateUniform(uEnvColor);
	LocateUniform(uPrimColor);
	LocateUniform(uFogColor);
	LocateUniform(uCenterColor);
	LocateUniform(uScaleColor);
	
	LocateUniform(uRenderState);
	
	LocateUniform(uTexScale);
	LocateUniform(uTexOffset[0]);
	LocateUniform(uTexOffset[1]);
	LocateUniform(uCacheShiftScale[0]);
	LocateUniform(uCacheShiftScale[1]);
	LocateUniform(uCacheScale[0]);
	LocateUniform(uCacheScale[1]);
	LocateUniform(uCacheOffset[0]);
	LocateUniform(uCacheOffset[1]);
	LocateUniform(uCacheFrameBuffer);

	if (config.enableHWLighting) {
		// locate lights uniforms
		char buf[32];
		for (u32 i = 0; i < 8; ++i) {
			sprintf(buf, "uLightDirection[%d]", i);
			m_uniforms.uLightDirection[i].loc = glGetUniformLocation(m_program, buf);
			sprintf(buf, "uLightColor[%d]", i);
			m_uniforms.uLightColor[i].loc = glGetUniformLocation(m_program, buf);
		}
	}
}

void GLSLCombiner::_locate_attributes() const {
	glBindAttribLocation(m_program, SC_POSITION, "aPosition");
	glBindAttribLocation(m_program, SC_COLOR, "aColor");
	glBindAttribLocation(m_program, SC_TEXCOORD0, "aTexCoord0");
	glBindAttribLocation(m_program, SC_TEXCOORD1, "aTexCoord1");
	glBindAttribLocation(m_program, SC_STSCALED, "aSTScaled");
	glBindAttribLocation(m_program, SC_NUMLIGHTS, "aNumLights");
}

void GLSLCombiner::Set() {
	combiner.usesT0 = (m_nInputs & ((1<<TEXEL0)|(1<<TEXEL0_ALPHA))) != 0;
	combiner.usesT1 = (m_nInputs & ((1<<TEXEL1)|(1<<TEXEL1_ALPHA))) != 0;
	combiner.usesLOD = (m_nInputs & (1<<LOD_FRACTION)) != 0;
	combiner.usesShadeColor = (m_nInputs & ((1<<SHADE)|(1<<SHADE_ALPHA))) != 0;

	combiner.vertex.color = COMBINED;
	combiner.vertex.alpha = COMBINED;
	combiner.vertex.secondaryColor = LIGHT;

	glUseProgram(m_program);

	_setIUniform(m_uniforms.uTex0, 0, true);
	_setIUniform(m_uniforms.uTex1, 1, true);

	UpdateRenderState(true);
	UpdateColors(true);
	UpdateTextureInfo(true);
	UpdateAlphaTestInfo(true);
	UpdateDepthInfo(true);
	UpdateLight(true);
}

void GLSLCombiner::UpdateRenderState(bool _bForce) {
	_setIUniform(m_uniforms.uRenderState, OGL.renderState, _bForce);
}

void GLSLCombiner::UpdateLight(bool _bForce) {
	if (config.enableHWLighting == 0)
		return;
	for (s32 i = 0; i <= gSP.numLights; ++i) {
		_setV3Uniform(m_uniforms.uLightDirection[i], &gSP.lights[i].x, _bForce);
		_setV3Uniform(m_uniforms.uLightColor[i], &gSP.lights[i].r, _bForce);
	}
}

void GLSLCombiner::UpdateColors(bool _bForce) {
	_setV4Uniform(m_uniforms.uEnvColor, &gDP.envColor.r, _bForce);
	_setV4Uniform(m_uniforms.uPrimColor, &gDP.primColor.r, _bForce);
	_setV4Uniform(m_uniforms.uCenterColor, &gDP.key.center.r, _bForce);
	_setV4Uniform(m_uniforms.uScaleColor, &gDP.key.scale.r, _bForce);
	_setIUniform(m_uniforms.uEnableFog, (config.enableFog != 0 && (gSP.geometryMode & G_FOG) != 0) ? 1 : 0, _bForce);
	if (m_uniforms.uEnableFog.val != 0) {
		_setFUniform(m_uniforms.uFogMultiplier, (float)gSP.fog.multiplier / 256.0f, _bForce);
		_setFUniform(m_uniforms.uFogOffset, (float)gSP.fog.offset / 256.0f, _bForce);
		_setV4Uniform(m_uniforms.uFogColor, &gDP.fogColor.r, _bForce);
	}
	_setFUniform(m_uniforms.uK4, gDP.convert.k4*0.0039215689f, _bForce);
	_setFUniform(m_uniforms.uK5, gDP.convert.k5*0.0039215689f, _bForce);

	if (combiner.usesLOD) {
		int uCalcLOD = gDP.otherMode.textureLOD == G_TL_LOD ? 1 : 0;
		_setIUniform(m_uniforms.uEnableLod, uCalcLOD, _bForce);
		if (uCalcLOD) {
			_setFUniform(m_uniforms.uLodXScale, OGL.scaleX, _bForce);
			_setFUniform(m_uniforms.uLodYScale, OGL.scaleY, _bForce);
			_setFUniform(m_uniforms.uMinLod, gDP.primColor.m, _bForce);
			_setIUniform(m_uniforms.uMaxTile, gSP.texture.level, _bForce);
			_setIUniform(m_uniforms.uTextureDetail, gDP.otherMode.textureDetail, _bForce);
		}
	}
	
	int nDither = (gDP.otherMode.alphaCompare == 3 && (gDP.otherMode.colorDither == 2 || gDP.otherMode.alphaDither == 2)) ? 1 : 0;
	_setIUniform(m_uniforms.uEnableDither, nDither, _bForce);

	if ((m_nInputs & (1<<NOISE)) + nDither > 0)
		_setFUniform(m_uniforms.uNoiseTime, (float)(rand()&255), _bForce);

	if (!_bForce)
		return;
	_setIUniform(m_uniforms.uFb8Bit, 0, _bForce);
	_setIUniform(m_uniforms.uFbFixedAlpha, 0, _bForce);
	_setIUniform(m_uniforms.uEnableDepth, 0, _bForce);
}

void GLSLCombiner::UpdateTextureInfo(bool _bForce) {
	_setIUniform(m_uniforms.uTexturePersp, gDP.otherMode.texturePersp, _bForce);
	_setFV2Uniform(m_uniforms.uTexScale, gSP.texture.scales, gSP.texture.scalet, _bForce);
	int nFB0 = 0, nFB1 = 0;
	if (combiner.usesT0) {
		if (gSP.textureTile[0])
			_setFV2Uniform(m_uniforms.uTexOffset[0], gSP.textureTile[0]->fuls, gSP.textureTile[0]->fult, _bForce);
		if (cache.current[0]) {
			_setFV2Uniform(m_uniforms.uCacheShiftScale[0], cache.current[0]->shiftScaleS, cache.current[0]->shiftScaleT, _bForce);
			_setFV2Uniform(m_uniforms.uCacheScale[0], cache.current[0]->scaleS, cache.current[0]->scaleT, _bForce);
			_setFV2Uniform(m_uniforms.uCacheOffset[0], cache.current[0]->offsetS, cache.current[0]->offsetT, _bForce);
			nFB0 = cache.current[0]->frameBufferTexture;
		}
	}

	if (combiner.usesT1) {
		if (gSP.textureTile[1])
			_setFV2Uniform(m_uniforms.uTexOffset[1], gSP.textureTile[1]->fuls, gSP.textureTile[1]->fult, _bForce);
		if (cache.current[1]) {
			_setFV2Uniform(m_uniforms.uCacheShiftScale[1], cache.current[1]->shiftScaleS, cache.current[1]->shiftScaleT, _bForce);
			_setFV2Uniform(m_uniforms.uCacheScale[1], cache.current[1]->scaleS, cache.current[1]->scaleT, _bForce);
			_setFV2Uniform(m_uniforms.uCacheOffset[1], cache.current[1]->offsetS, cache.current[1]->offsetT, _bForce);
			nFB1 = cache.current[1]->frameBufferTexture;
		}
	}
	_setIV2Uniform(m_uniforms.uCacheFrameBuffer, nFB0, nFB1, _bForce);
	_setFUniform(m_uniforms.uPrimLod, gDP.primColor.l, _bForce);
}

void GLSLCombiner::UpdateFBInfo(bool _bForce) {
	int nFb8bitMode = 0, nFbFixedAlpha = 0;
	if (cache.current[0] != NULL && cache.current[0]->frameBufferTexture == TRUE) {
		if (cache.current[0]->size == G_IM_SIZ_8b) {
			nFb8bitMode |= 1;
			if (gDP.otherMode.imageRead == 0)
				nFbFixedAlpha |= 1;
		}
	}
	if (cache.current[1] != NULL && cache.current[1]->frameBufferTexture == TRUE) {
		if (cache.current[1]->size == G_IM_SIZ_8b) {
			nFb8bitMode |= 2;
			if (gDP.otherMode.imageRead == 0)
				nFbFixedAlpha |= 2;
		}
	}
	_setIUniform(m_uniforms.uFb8Bit, nFb8bitMode, _bForce);
	_setIUniform(m_uniforms.uFbFixedAlpha, nFbFixedAlpha, _bForce);
}

void GLSLCombiner::UpdateDepthInfo(bool _bForce) {
	if (!OGL.bImageTexture)
		return;

	if (frameBuffer.top == NULL || frameBuffer.top->pDepthBuffer == NULL)
		return;

	const int nDepthEnabled = (gSP.geometryMode & G_ZBUFFER) > 0 ? 1 : 0;
	_setIUniform(m_uniforms.uEnableDepth, nDepthEnabled, _bForce);
	if (nDepthEnabled == 0) {
		_setIUniform(m_uniforms.uEnableDepthCompare, 0, _bForce);
		_setIUniform(m_uniforms.uEnableDepthUpdate, 0, _bForce);
	} else {
		_setIUniform(m_uniforms.uEnableDepthCompare, gDP.otherMode.depthCompare, _bForce);
		_setIUniform(m_uniforms.uEnableDepthUpdate, gDP.otherMode.depthUpdate, _bForce);
	}
	_setIUniform(m_uniforms.uDepthMode, gDP.otherMode.depthMode, _bForce);
	_setFUniform(m_uniforms.uDepthScale, gSP.viewport.vscale[2]*32768.0f, _bForce);
	_setFUniform(m_uniforms.uDepthTrans, gSP.viewport.vtrans[2]*32768.0f, _bForce);
}

void GLSLCombiner::UpdateAlphaTestInfo(bool _bForce) {
	if ((gDP.otherMode.alphaCompare == G_AC_THRESHOLD) && !(gDP.otherMode.alphaCvgSel))	{
		_setIUniform(m_uniforms.uEnableAlphaTest, 1, _bForce);
		_setFUniform(m_uniforms.uAlphaTestValue, gDP.blendColor.a, _bForce);
	} else if (gDP.otherMode.cvgXAlpha)	{
		_setIUniform(m_uniforms.uEnableAlphaTest, 1, _bForce);
		_setFUniform(m_uniforms.uAlphaTestValue, 0.5f, _bForce);
	} else {
		_setIUniform(m_uniforms.uEnableAlphaTest, 0, _bForce);
		_setFUniform(m_uniforms.uAlphaTestValue, 0.0f, _bForce);
	}
}

#ifndef GLES2
void GLSL_RenderDepth() {
	if (!OGL.bImageTexture)
		return;
#if 0
	glBindFramebuffer(GL_READ_FRAMEBUFFER, g_zbuf_fbo);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
	glDrawBuffer( GL_FRONT );
	glBlitFramebuffer(
		0, 0, OGL.width, OGL.height,
		0, OGL.heightOffset, OGL.width, OGL.heightOffset + OGL.height,
		GL_COLOR_BUFFER_BIT, GL_LINEAR
	);
	glDrawBuffer( GL_BACK );
	glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, frameBuffer.top != NULL ? frameBuffer.top->fbo : 0);
#else
	if (frameBuffer.top == NULL || frameBuffer.top->pDepthBuffer == NULL)
		return;
	glBindImageTexture(depthImageUnit, 0, 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA32F);
	glPushAttrib( GL_ENABLE_BIT | GL_VIEWPORT_BIT );

	glActiveTexture( GL_TEXTURE0 );
	glBindTexture(GL_TEXTURE_2D, frameBuffer.top->pDepthBuffer->depth_texture->glName);
//	glBindTexture(GL_TEXTURE_2D, g_zlut_tex);

	Combiner_SetCombine( EncodeCombineMode( 0, 0, 0, TEXEL0, 0, 0, 0, 1, 0, 0, 0, TEXEL0, 0, 0, 0, 1 ) );

			glDisable( GL_BLEND );
			glDisable( GL_ALPHA_TEST );
			glDisable( GL_DEPTH_TEST );
			glDisable( GL_CULL_FACE );
			glDisable( GL_POLYGON_OFFSET_FILL );

			glMatrixMode( GL_PROJECTION );
			glLoadIdentity();
			glOrtho( 0, OGL.width, 0, OGL.height, -1.0f, 1.0f );
			glViewport( 0, OGL.heightOffset, OGL.width, OGL.height );
			glDisable( GL_SCISSOR_TEST );

			float u1, v1;

			u1 = 1.0;
			v1 = 1.0;

			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
#ifdef _WINDOWS
			glDrawBuffer( GL_FRONT );
#else
			glDrawBuffer( GL_BACK );
#endif
			glBegin(GL_QUADS);
 				glTexCoord2f( 0.0f, 0.0f );
				glVertex2f( 0.0f, 0.0f );

				glTexCoord2f( 0.0f, v1 );
				glVertex2f( 0.0f, (GLfloat)OGL.height );

 				glTexCoord2f( u1,  v1 );
				glVertex2f( (GLfloat)OGL.width, (GLfloat)OGL.height );

 				glTexCoord2f( u1, 0.0f );
				glVertex2f( (GLfloat)OGL.width, 0.0f );
			glEnd();
#ifdef _WINDOWS
			glDrawBuffer( GL_BACK );
#else
			OGL_SwapBuffers();
#endif
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, frameBuffer.top->fbo);
			glBindImageTexture(depthImageUnit, frameBuffer.top->pDepthBuffer->depth_texture->glName, 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA32F);


			glLoadIdentity();
			glPopAttrib();

			gSP.changed |= CHANGED_TEXTURE | CHANGED_VIEWPORT;
			gDP.changed |= CHANGED_COMBINE;
#endif
}

void GLS_SetShadowMapCombiner() {

	if (!OGL.bImageTexture)
		return;

	if (g_paletteCRC256 != gDP.paletteCRC256) {
		g_paletteCRC256 = gDP.paletteCRC256;
		u16 palette[256];
		u16 *src = (u16*)&TMEM[256];
		for (int i = 0; i < 256; ++i)
			palette[i] = swapword(src[i*4]);
		glBindImageTexture(TlutImageUnit, 0, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16UI);
		glBindTexture(GL_TEXTURE_1D, g_tlut_tex);
		glTexSubImage1D(GL_TEXTURE_1D, 0, 0, 256, GL_RED, GL_UNSIGNED_SHORT, palette);
		glBindTexture(GL_TEXTURE_1D, 0);
		glBindImageTexture(TlutImageUnit, g_tlut_tex, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R16UI);
	}

	glUseProgram(g_draw_shadow_map_program);
	int loc = glGetUniformLocation(g_draw_shadow_map_program, "uFogColor");
	glUniform4fv(loc, 1, &gDP.fogColor.r);

	gDP.changed |= CHANGED_COMBINE;
}
#endif // GLES2
