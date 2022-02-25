/* Copyright (C) 2022 Wildfire Games.
 * This file is part of 0 A.D.
 *
 * 0 A.D. is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * 0 A.D. is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with 0 A.D.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "precompiled.h"

#include "ShaderManager.h"

#include "graphics/PreprocessorWrapper.h"
#include "graphics/ShaderTechnique.h"
#include "lib/config2.h"
#include "lib/hash.h"
#include "lib/timer.h"
#include "lib/utf8.h"
#include "ps/CLogger.h"
#include "ps/CStrIntern.h"
#include "ps/Filesystem.h"
#include "ps/Profile.h"
#include "ps/XML/Xeromyces.h"
#include "ps/XML/XMLWriter.h"
#include "ps/VideoMode.h"
#include "renderer/backend/gl/Device.h"
#include "renderer/Renderer.h"
#include "renderer/RenderingOptions.h"

#define USE_SHADER_XML_VALIDATION 1

#if USE_SHADER_XML_VALIDATION
# include "ps/XML/RelaxNG.h"
#endif

#include <vector>

TIMER_ADD_CLIENT(tc_ShaderValidation);

CShaderManager::CShaderManager()
{
#if USE_SHADER_XML_VALIDATION
	{
		TIMER_ACCRUE(tc_ShaderValidation);

		if (!CXeromyces::AddValidator(g_VFS, "shader", "shaders/program.rng"))
			LOGERROR("CShaderManager: failed to load grammar shaders/program.rng");
	}
#endif

	// Allow hotloading of textures
	RegisterFileReloadFunc(ReloadChangedFileCB, this);
}

CShaderManager::~CShaderManager()
{
	UnregisterFileReloadFunc(ReloadChangedFileCB, this);
}

CShaderProgramPtr CShaderManager::LoadProgram(const char* name, const CShaderDefines& defines)
{
	CacheKey key = { name, defines };
	std::map<CacheKey, CShaderProgramPtr>::iterator it = m_ProgramCache.find(key);
	if (it != m_ProgramCache.end())
		return it->second;

	CShaderProgramPtr program;
	if (!NewProgram(name, defines, program))
	{
		LOGERROR("Failed to load shader '%s'", name);
		program = CShaderProgramPtr();
	}

	m_ProgramCache[key] = program;
	return program;
}

static GLenum ParseAttribSemantics(const CStr& str)
{
	// Map known semantics onto the attribute locations documented by NVIDIA
	if (str == "gl_Vertex") return 0;
	if (str == "gl_Normal") return 2;
	if (str == "gl_Color") return 3;
	if (str == "gl_SecondaryColor") return 4;
	if (str == "gl_FogCoord") return 5;
	if (str == "gl_MultiTexCoord0") return 8;
	if (str == "gl_MultiTexCoord1") return 9;
	if (str == "gl_MultiTexCoord2") return 10;
	if (str == "gl_MultiTexCoord3") return 11;
	if (str == "gl_MultiTexCoord4") return 12;
	if (str == "gl_MultiTexCoord5") return 13;
	if (str == "gl_MultiTexCoord6") return 14;
	if (str == "gl_MultiTexCoord7") return 15;

	// Define some arbitrary names for user-defined attribute locations
	// that won't conflict with any standard semantics
	if (str == "CustomAttribute0") return 1;
	if (str == "CustomAttribute1") return 6;
	if (str == "CustomAttribute2") return 7;

	debug_warn("Invalid attribute semantics");
	return 0;
}

bool CShaderManager::NewProgram(const char* name, const CShaderDefines& baseDefines, CShaderProgramPtr& program)
{
	PROFILE2("loading shader");
	PROFILE2_ATTR("name: %s", name);

	VfsPath xmlFilename = L"shaders/" + wstring_from_utf8(name) + L".xml";

	CXeromyces XeroFile;
	PSRETURN ret = XeroFile.Load(g_VFS, xmlFilename);
	if (ret != PSRETURN_OK)
		return false;

#if USE_SHADER_XML_VALIDATION
	{
		TIMER_ACCRUE(tc_ShaderValidation);

		// Serialize the XMB data and pass it to the validator
		XMLWriter_File shaderFile;
		shaderFile.SetPrettyPrint(false);
		shaderFile.XMB(XeroFile);
		bool ok = CXeromyces::ValidateEncoded("shader", name, shaderFile.GetOutput());
		if (!ok)
			return false;
	}
#endif

	// Define all the elements and attributes used in the XML file
#define EL(x) int el_##x = XeroFile.GetElementID(#x)
#define AT(x) int at_##x = XeroFile.GetAttributeID(#x)
	EL(attrib);
	EL(define);
	EL(fragment);
	EL(stream);
	EL(uniform);
	EL(vertex);
	AT(file);
	AT(if);
	AT(loc);
	AT(name);
	AT(semantics);
	AT(type);
	AT(value);
#undef AT
#undef EL

	CPreprocessorWrapper preprocessor;
	preprocessor.AddDefines(baseDefines);

	XMBElement root = XeroFile.GetRoot();

	VfsPath vertexFile;
	VfsPath fragmentFile;
	CShaderDefines defines = baseDefines;
	std::map<CStrIntern, int> vertexUniforms;
	std::map<CStrIntern, CShaderProgram::frag_index_pair_t> fragmentUniforms;
	std::map<CStrIntern, int> vertexAttribs;
	int streamFlags = 0;

	XERO_ITER_EL(root, Child)
	{
		if (Child.GetNodeName() == el_define)
		{
			defines.Add(CStrIntern(Child.GetAttributes().GetNamedItem(at_name)), CStrIntern(Child.GetAttributes().GetNamedItem(at_value)));
		}
		else if (Child.GetNodeName() == el_vertex)
		{
			vertexFile = L"shaders/" + Child.GetAttributes().GetNamedItem(at_file).FromUTF8();

			XERO_ITER_EL(Child, Param)
			{
				XMBAttributeList Attrs = Param.GetAttributes();

				CStr cond = Attrs.GetNamedItem(at_if);
				if (!cond.empty() && !preprocessor.TestConditional(cond))
					continue;

				if (Param.GetNodeName() == el_uniform)
				{
					vertexUniforms[CStrIntern(Attrs.GetNamedItem(at_name))] = Attrs.GetNamedItem(at_loc).ToInt();
				}
				else if (Param.GetNodeName() == el_stream)
				{
					CStr StreamName = Attrs.GetNamedItem(at_name);
					if (StreamName == "pos")
						streamFlags |= STREAM_POS;
					else if (StreamName == "normal")
						streamFlags |= STREAM_NORMAL;
					else if (StreamName == "color")
						streamFlags |= STREAM_COLOR;
					else if (StreamName == "uv0")
						streamFlags |= STREAM_UV0;
					else if (StreamName == "uv1")
						streamFlags |= STREAM_UV1;
					else if (StreamName == "uv2")
						streamFlags |= STREAM_UV2;
					else if (StreamName == "uv3")
						streamFlags |= STREAM_UV3;
				}
				else if (Param.GetNodeName() == el_attrib)
				{
					int attribLoc = ParseAttribSemantics(Attrs.GetNamedItem(at_semantics));
					vertexAttribs[CStrIntern(Attrs.GetNamedItem(at_name))] = attribLoc;
				}
			}
		}
		else if (Child.GetNodeName() == el_fragment)
		{
			fragmentFile = L"shaders/" + Child.GetAttributes().GetNamedItem(at_file).FromUTF8();

			XERO_ITER_EL(Child, Param)
			{
				XMBAttributeList Attrs = Param.GetAttributes();

				CStr cond = Attrs.GetNamedItem(at_if);
				if (!cond.empty() && !preprocessor.TestConditional(cond))
					continue;

				if (Param.GetNodeName() == el_uniform)
				{
					// A somewhat incomplete listing, missing "shadow" and "rect" versions
					// which are interpreted as 2D (NB: our shadowmaps may change
					// type based on user config).
					GLenum type = GL_TEXTURE_2D;
					CStr t = Attrs.GetNamedItem(at_type);
					if (t == "sampler1D")
#if CONFIG2_GLES
						debug_warn(L"sampler1D not implemented on GLES");
#else
						type = GL_TEXTURE_1D;
#endif
					else if (t == "sampler2D")
						type = GL_TEXTURE_2D;
					else if (t == "sampler3D")
#if CONFIG2_GLES
						debug_warn(L"sampler3D not implemented on GLES");
#else
						type = GL_TEXTURE_3D;
#endif
					else if (t == "samplerCube")
						type = GL_TEXTURE_CUBE_MAP;

					fragmentUniforms[CStrIntern(Attrs.GetNamedItem(at_name))] =
						std::make_pair(Attrs.GetNamedItem(at_loc).ToInt(), type);
				}
			}
		}
	}

	if (root.GetAttributes().GetNamedItem(at_type) == "glsl")
		program = CShaderProgramPtr(CShaderProgram::ConstructGLSL(vertexFile, fragmentFile, defines, vertexAttribs, streamFlags));
	else
		program = CShaderProgramPtr(CShaderProgram::ConstructARB(vertexFile, fragmentFile, defines, vertexUniforms, fragmentUniforms, streamFlags));

	program->Reload();

//	m_HotloadFiles[xmlFilename].insert(program); // TODO: should reload somehow when the XML changes
	for (const VfsPath& path : program->GetFileDependencies())
		AddProgramFileDependency(program, path);

	return true;
}

size_t CShaderManager::EffectCacheKeyHash::operator()(const EffectCacheKey& key) const
{
	size_t hash = 0;
	hash_combine(hash, key.name.GetHash());
	hash_combine(hash, key.defines.GetHash());
	return hash;
}

bool CShaderManager::EffectCacheKey::operator==(const EffectCacheKey& b) const
{
	return name == b.name && defines == b.defines;
}

CShaderTechniquePtr CShaderManager::LoadEffect(CStrIntern name)
{
	return LoadEffect(name, CShaderDefines());
}

CShaderTechniquePtr CShaderManager::LoadEffect(CStrIntern name, const CShaderDefines& defines)
{
	// Return the cached effect, if there is one
	EffectCacheKey key = { name, defines };
	EffectCacheMap::iterator it = m_EffectCache.find(key);
	if (it != m_EffectCache.end())
		return it->second;

	// First time we've seen this key, so construct a new effect:
	CShaderTechniquePtr tech(new CShaderTechnique());
	if (!NewEffect(name.c_str(), defines, tech))
	{
		LOGERROR("Failed to load effect '%s'", name.c_str());
		tech = CShaderTechniquePtr();
	}

	m_EffectCache[key] = tech;
	return tech;
}

bool CShaderManager::NewEffect(const char* name, const CShaderDefines& baseDefines, CShaderTechniquePtr& tech)
{
	PROFILE2("loading effect");
	PROFILE2_ATTR("name: %s", name);

	VfsPath xmlFilename = L"shaders/effects/" + wstring_from_utf8(name) + L".xml";

	CXeromyces XeroFile;
	PSRETURN ret = XeroFile.Load(g_VFS, xmlFilename);
	if (ret != PSRETURN_OK)
		return false;

	// Define all the elements and attributes used in the XML file
#define EL(x) int el_##x = XeroFile.GetElementID(#x)
#define AT(x) int at_##x = XeroFile.GetAttributeID(#x)
	EL(blend);
	EL(color);
	EL(cull);
	EL(define);
	EL(depth);
	EL(pass);
	EL(polygon);
	EL(require);
	EL(sort_by_distance);
	EL(stencil);
	AT(compare);
	AT(constant);
	AT(context);
	AT(depth_fail);
	AT(dst);
	AT(fail);
	AT(front_face);
	AT(func);
	AT(mask);
	AT(mask_read);
	AT(mask_red);
	AT(mask_green);
	AT(mask_blue);
	AT(mask_alpha);
	AT(mode);
	AT(name);
	AT(op);
	AT(pass);
	AT(reference);
	AT(shader);
	AT(shaders);
	AT(src);
	AT(test);
	AT(value);
#undef AT
#undef EL

	// Prepare the preprocessor for conditional tests
	CPreprocessorWrapper preprocessor;
	preprocessor.AddDefines(baseDefines);

	XMBElement Root = XeroFile.GetRoot();

	// Find all the techniques that we can use, and their preference

	std::vector<XMBElement> usableTechs;

	XERO_ITER_EL(Root, Technique)
	{
		bool isUsable = true;
		XERO_ITER_EL(Technique, Child)
		{
			XMBAttributeList Attrs = Child.GetAttributes();

			// TODO: require should be an attribute of the tech and not its child.
			if (Child.GetNodeName() == el_require)
			{
				if (Attrs.GetNamedItem(at_shaders) == "arb")
				{
					if (g_VideoMode.GetBackend() != CVideoMode::Backend::GL_ARB ||
						!g_VideoMode.GetBackendDevice()->GetCapabilities().ARBShaders)
					{
						isUsable = false;
					}
				}
				else if (Attrs.GetNamedItem(at_shaders) == "glsl")
				{
					if (g_VideoMode.GetBackend() != CVideoMode::Backend::GL)
						isUsable = false;
				}
				else if (!Attrs.GetNamedItem(at_context).empty())
				{
					CStr cond = Attrs.GetNamedItem(at_context);
					if (!preprocessor.TestConditional(cond))
						isUsable = false;
				}
			}
		}

		if (isUsable)
			usableTechs.emplace_back(Technique);
	}

	if (usableTechs.empty())
	{
		debug_warn(L"Can't find a usable technique");
		return false;
	}

	CShaderDefines techDefines = baseDefines;
	XERO_ITER_EL(usableTechs[0], Child)
	{
		if (Child.GetNodeName() == el_define)
		{
			techDefines.Add(CStrIntern(Child.GetAttributes().GetNamedItem(at_name)), CStrIntern(Child.GetAttributes().GetNamedItem(at_value)));
		}
		else if (Child.GetNodeName() == el_sort_by_distance)
		{
			tech->SetSortByDistance(true);
		}
	}
	// We don't want to have a shader context depending on the order of define and
	// pass tags.
	// TODO: we might want to implement that in a proper way via splitting passes
	// and tags in different groups in XML.
	std::vector<CShaderPass> techPasses;
	XERO_ITER_EL(usableTechs[0], Child)
	{
		if (Child.GetNodeName() == el_pass)
		{
			CShaderDefines passDefines = techDefines;

			CShaderPass pass;
			Renderer::Backend::GraphicsPipelineStateDesc passPipelineStateDesc =
				Renderer::Backend::MakeDefaultGraphicsPipelineStateDesc();

			XERO_ITER_EL(Child, Element)
			{
				if (Element.GetNodeName() == el_define)
				{
					passDefines.Add(CStrIntern(Element.GetAttributes().GetNamedItem(at_name)), CStrIntern(Element.GetAttributes().GetNamedItem(at_value)));
				}
				else if (Element.GetNodeName() == el_blend)
				{
					passPipelineStateDesc.blendState.enabled = true;
					passPipelineStateDesc.blendState.srcColorBlendFactor = passPipelineStateDesc.blendState.srcAlphaBlendFactor =
						Renderer::Backend::ParseBlendFactor(Element.GetAttributes().GetNamedItem(at_src));
					passPipelineStateDesc.blendState.dstColorBlendFactor = passPipelineStateDesc.blendState.dstAlphaBlendFactor =
						Renderer::Backend::ParseBlendFactor(Element.GetAttributes().GetNamedItem(at_dst));
					if (!Element.GetAttributes().GetNamedItem(at_op).empty())
					{
						passPipelineStateDesc.blendState.colorBlendOp = passPipelineStateDesc.blendState.alphaBlendOp =
							Renderer::Backend::ParseBlendOp(Element.GetAttributes().GetNamedItem(at_op));
					}
					if (!Element.GetAttributes().GetNamedItem(at_constant).empty())
					{
						if (!passPipelineStateDesc.blendState.constant.ParseString(
								Element.GetAttributes().GetNamedItem(at_constant)))
						{
							LOGERROR("Failed to parse blend constant: %s",
								Element.GetAttributes().GetNamedItem(at_constant).c_str());
						}
					}
				}
				else if (Element.GetNodeName() == el_color)
				{
					passPipelineStateDesc.blendState.colorWriteMask = 0;
				#define MASK_CHANNEL(ATTRIBUTE, VALUE) \
					if (Element.GetAttributes().GetNamedItem(ATTRIBUTE) == "TRUE") \
						passPipelineStateDesc.blendState.colorWriteMask |= Renderer::Backend::ColorWriteMask::VALUE

					MASK_CHANNEL(at_mask_red, RED);
					MASK_CHANNEL(at_mask_green, GREEN);
					MASK_CHANNEL(at_mask_blue, BLUE);
					MASK_CHANNEL(at_mask_alpha, ALPHA);
				#undef MASK_CHANNEL
				}
				else if (Element.GetNodeName() == el_cull)
				{
					if (!Element.GetAttributes().GetNamedItem(at_mode).empty())
					{
						passPipelineStateDesc.rasterizationState.cullMode =
							Renderer::Backend::ParseCullMode(Element.GetAttributes().GetNamedItem(at_mode));
					}
					if (!Element.GetAttributes().GetNamedItem(at_front_face).empty())
					{
						passPipelineStateDesc.rasterizationState.frontFace =
							Renderer::Backend::ParseFrontFace(Element.GetAttributes().GetNamedItem(at_front_face));
					}
				}
				else if (Element.GetNodeName() == el_depth)
				{
					if (!Element.GetAttributes().GetNamedItem(at_test).empty())
					{
						passPipelineStateDesc.depthStencilState.depthTestEnabled =
							Element.GetAttributes().GetNamedItem(at_test) == "TRUE";
					}

					if (!Element.GetAttributes().GetNamedItem(at_func).empty())
					{
						passPipelineStateDesc.depthStencilState.depthCompareOp =
							Renderer::Backend::ParseCompareOp(Element.GetAttributes().GetNamedItem(at_func));
					}

					if (!Element.GetAttributes().GetNamedItem(at_mask).empty())
					{
						passPipelineStateDesc.depthStencilState.depthWriteEnabled =
							Element.GetAttributes().GetNamedItem(at_mask) == "true";
					}
				}
				else if (Element.GetNodeName() == el_polygon)
				{
					if (!Element.GetAttributes().GetNamedItem(at_mode).empty())
					{
						passPipelineStateDesc.rasterizationState.polygonMode =
							Renderer::Backend::ParsePolygonMode(Element.GetAttributes().GetNamedItem(at_mode));
					}
				}
				else if (Element.GetNodeName() == el_stencil)
				{
					if (!Element.GetAttributes().GetNamedItem(at_test).empty())
					{
						passPipelineStateDesc.depthStencilState.stencilTestEnabled =
							Element.GetAttributes().GetNamedItem(at_test) == "TRUE";
					}

					if (!Element.GetAttributes().GetNamedItem(at_reference).empty())
					{
						passPipelineStateDesc.depthStencilState.stencilReference =
							Element.GetAttributes().GetNamedItem(at_reference).ToULong();
					}
					if (!Element.GetAttributes().GetNamedItem(at_mask_read).empty())
					{
						passPipelineStateDesc.depthStencilState.stencilReadMask =
							Element.GetAttributes().GetNamedItem(at_mask_read).ToULong();
					}
					if (!Element.GetAttributes().GetNamedItem(at_mask).empty())
					{
						passPipelineStateDesc.depthStencilState.stencilWriteMask =
							Element.GetAttributes().GetNamedItem(at_mask).ToULong();
					}

					if (!Element.GetAttributes().GetNamedItem(at_compare).empty())
					{
						passPipelineStateDesc.depthStencilState.stencilFrontFace.compareOp =
							passPipelineStateDesc.depthStencilState.stencilBackFace.compareOp =
								Renderer::Backend::ParseCompareOp(Element.GetAttributes().GetNamedItem(at_compare));
					}
					if (!Element.GetAttributes().GetNamedItem(at_fail).empty())
					{
						passPipelineStateDesc.depthStencilState.stencilFrontFace.failOp =
							passPipelineStateDesc.depthStencilState.stencilBackFace.failOp =
								Renderer::Backend::ParseStencilOp(Element.GetAttributes().GetNamedItem(at_fail));
					}
					if (!Element.GetAttributes().GetNamedItem(at_pass).empty())
					{
						passPipelineStateDesc.depthStencilState.stencilFrontFace.passOp =
							passPipelineStateDesc.depthStencilState.stencilBackFace.passOp =
							Renderer::Backend::ParseStencilOp(Element.GetAttributes().GetNamedItem(at_pass));
					}
					if (!Element.GetAttributes().GetNamedItem(at_depth_fail).empty())
					{
						passPipelineStateDesc.depthStencilState.stencilFrontFace.depthFailOp =
							passPipelineStateDesc.depthStencilState.stencilBackFace.depthFailOp =
							Renderer::Backend::ParseStencilOp(Element.GetAttributes().GetNamedItem(at_depth_fail));
					}
				}
			}

			pass.SetPipelineStateDesc(passPipelineStateDesc);

			// Load the shader program after we've read all the possibly-relevant <define>s
			pass.SetShader(LoadProgram(Child.GetAttributes().GetNamedItem(at_shader).c_str(), passDefines));

			techPasses.emplace_back(std::move(pass));
		}
	}

	tech->SetPasses(std::move(techPasses));

	return true;
}

size_t CShaderManager::GetNumEffectsLoaded() const
{
	return m_EffectCache.size();
}

/*static*/ Status CShaderManager::ReloadChangedFileCB(void* param, const VfsPath& path)
{
	return static_cast<CShaderManager*>(param)->ReloadChangedFile(path);
}

Status CShaderManager::ReloadChangedFile(const VfsPath& path)
{
	// Find all shaders using this file
	HotloadFilesMap::iterator files = m_HotloadFiles.find(path);
	if (files == m_HotloadFiles.end())
		return INFO::OK;

	// Reload all shaders using this file
	for (const std::weak_ptr<CShaderProgram>& ptr : files->second)
		if (std::shared_ptr<CShaderProgram> program = ptr.lock())
			program->Reload();

	// TODO: hotloading changes to shader XML files and effect XML files would be nice

	return INFO::OK;
}

void CShaderManager::AddProgramFileDependency(const CShaderProgramPtr& program, const VfsPath& path)
{
	m_HotloadFiles[path].insert(program);
}
