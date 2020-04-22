#include "stdafx.h"
#include "ShadedMesh.hpp"
#include "Application.hpp"
#include "lua.hpp"

ShadedMesh::ShadedMesh() {
	m_material = g_application->LoadMaterial("guiTex");
	m_material->opaque = false;
	auto mainTex = TextureRes::Create(g_gl);
	SetParam("mainTex", mainTex);
	SetParam("color", Color::White);
	m_textures.Add("mainTex", mainTex);
	m_mesh = MeshRes::Create(g_gl);
	m_mesh->SetPrimitiveType(Graphics::PrimitiveType::TriangleList);
}

ShadedMesh::ShadedMesh(const String& name) {
	m_material = g_application->LoadMaterial(name);
	m_material->opaque = false;
	m_mesh = MeshRes::Create(g_gl);
	m_mesh->SetPrimitiveType(Graphics::PrimitiveType::TriangleList);
}

ShadedMesh::ShadedMesh(const String& name, const String& path) {
	m_material = g_application->LoadMaterial(name, path);
	m_material->opaque = false;
	m_mesh = MeshRes::Create(g_gl);
	m_mesh->SetPrimitiveType(Graphics::PrimitiveType::TriangleList);
}

void ShadedMesh::Draw() {
	auto rq = g_application->GetRenderQueueBase();
	rq->DrawScissored(g_application->GetCurrentGUIScissor() ,g_application->GetCurrentGUITransform(), m_mesh, m_material, m_params);
}

void ShadedMesh::SetData(Vector<MeshGenerators::SimpleVertex>& data) {
	m_mesh->SetData(data);
}

void ShadedMesh::AddTexture(const String& name, const String& file) {
	auto newTex = g_application->LoadTexture(file, true);
	m_textures.Add(name, newTex);
	SetParam(name, newTex);
}

void ShadedMesh::AddSkinTexture(const String& name, const String& file) {
	auto newTex = g_application->LoadTexture(file);
	m_textures.Add(name, newTex);
	SetParam(name, newTex);
}

void ShadedMesh::SetBlendMode(const MaterialBlendMode& mode)
{
	m_material->blendMode = mode;
}

void ShadedMesh::SetPrimitiveType(const PrimitiveType& type)
{
	m_mesh->SetPrimitiveType(type);
}

void ShadedMesh::SetOpaque(bool opaque)
{
	m_material->opaque = opaque;
}

int lSetData(lua_State* L) {
	ShadedMesh* object = *static_cast<ShadedMesh**>(lua_touserdata(L, 1));
	Vector<MeshGenerators::SimpleVertex> newData;

	lua_pushvalue(L, 2);
	lua_pushnil(L);
	while (lua_next(L, -2) != 0)
	{
		lua_pushvalue(L, -1);
		lua_pushnil(L);
		lua_next(L, -2);
		float x, y, u, v;
		{

			lua_pushvalue(L, -1);
			lua_pushnil(L);
			{
				lua_next(L, -2);
				x = luaL_checknumber(L, -1);
				lua_pop(L, 1);
				lua_next(L, -2);
				y = luaL_checknumber(L, -1);
				lua_pop(L, 2);
			}
		}
		lua_pop(L, 2);
		lua_next(L, -2);
		{

			lua_pushvalue(L, -1);
			lua_pushnil(L);
			lua_next(L, -2);
			{
				u = luaL_checknumber(L, -1);
				lua_pop(L, 1);
				lua_next(L, -2);
				v = luaL_checknumber(L, -1);
				lua_pop(L, 2);
			}
		}
		lua_pop(L, 5);

		MeshGenerators::SimpleVertex newVert;
		newVert.pos = { x, y, 0.0f};
		newVert.tex = { u, v };
		newData.Add(newVert);
	}
	object->SetData(newData);
	return 0;
}

int lDraw(lua_State* L) {
	ShadedMesh** userdata = static_cast<ShadedMesh**>(lua_touserdata(L, 1));
	if (userdata)
	{
		ShadedMesh* object = *userdata;
		object->Draw();
	}
	else {
		luaL_error(L, "null userdata");
	}
	return 0;
}

int lAddTexture(lua_State* L) {
	ShadedMesh* object = *static_cast<ShadedMesh**>(lua_touserdata(L, 1));
	object->AddTexture(luaL_checkstring(L, 2), luaL_checkstring(L, 3));
	return 0;
}

int lAddSkinTexture(lua_State* L) {
	ShadedMesh* object = *static_cast<ShadedMesh**>(lua_touserdata(L, 1));
	object->AddSkinTexture(luaL_checkstring(L, 2), luaL_checkstring(L, 3));
	return 0;
}

int lSetBlendMode(lua_State* L) {
	ShadedMesh* object = *static_cast<ShadedMesh**>(lua_touserdata(L, 1));
	MaterialBlendMode mode = (MaterialBlendMode)luaL_checkinteger(L, 2);
	object->SetBlendMode(mode);
	return 0;
}

int lSetPrimitiveType(lua_State* L) {
	ShadedMesh* object = *static_cast<ShadedMesh**>(lua_touserdata(L, 1));
	PrimitiveType type = (PrimitiveType)luaL_checkinteger(L, 2);
	object->SetPrimitiveType(type);
	return 0;
}

int lSetOpaque(lua_State* L) {
	ShadedMesh* object = *static_cast<ShadedMesh**>(lua_touserdata(L, 1));
	bool opaque = lua_toboolean(L, 2);
	object->SetOpaque(opaque);
	return 0;
}

int lSetParam(lua_State* L) {
	ShadedMesh* object = *static_cast<ShadedMesh**>(lua_touserdata(L, 1));
	if (lua_isinteger(L, 3)) {
		String name = luaL_checkstring(L, 2);
		int value = luaL_checkinteger(L, 3);
		object->SetParam(name, value);
		return 0;
	}
	else if (lua_isnumber(L, 3)) {
		String name = luaL_checkstring(L, 2);
		float value = luaL_checknumber(L, 3);
		object->SetParam(name, value);
		return 0;
	}
	return 0;
}

int lSetParamVec3(lua_State* L)
{
	ShadedMesh* object = *static_cast<ShadedMesh**>(lua_touserdata(L, 1));
	String name = luaL_checkstring(L, 2);
	float x, y, z;
	x = luaL_checknumber(L, 3);
	y = luaL_checknumber(L, 4);
	z = luaL_checknumber(L, 5);
	Vector3 vec = { x,y,z };
	object->SetParam(name, vec);

	return 0;
}

int lSetParamVec2(lua_State* L)
{
	ShadedMesh* object = *static_cast<ShadedMesh**>(lua_touserdata(L, 1));
	String name = luaL_checkstring(L, 2);
	if (lua_isinteger(L, 3)) {
		int x, y;
		x = luaL_checkinteger(L, 3);
		y = luaL_checkinteger(L, 4);
		Vector2i vec = { x,y };
		object->SetParam(name, vec);
	}
	else if (lua_isnumber(L, 3)) {
		float x, y;
		x = luaL_checknumber(L, 3);
		y = luaL_checknumber(L, 4);
		Vector2 vec = { x,y };
		object->SetParam(name, vec);
	}

	return 0;
}

int lSetParamVec4(lua_State* L)
{
	ShadedMesh* object = *static_cast<ShadedMesh**>(lua_touserdata(L, 1));
	String name = luaL_checkstring(L, 2);
	float x, y, z, w;
	x = luaL_checknumber(L, 3);
	y = luaL_checknumber(L, 4);
	z = luaL_checknumber(L, 5);
	w = luaL_checknumber(L, 6);
	Vector4 vec = { x,y,z,w };
	object->SetParam(name, vec);
	

	return 0;
}



int __index(lua_State* L) {
	ShadedMesh* object = *static_cast<ShadedMesh**>(lua_touserdata(L, 1));
	String fname = lua_tostring(L, 2);
	Map<String, lua_CFunction> fmap;
	Map<String, int> constMap;
	fmap.Add("Draw", lDraw);
	fmap.Add("AddTexture", lAddTexture);
	fmap.Add("AddSkinTexture", lAddSkinTexture);
	fmap.Add("SetParam", lSetParam);
	fmap.Add("SetParamVec2", lSetParamVec2);
	fmap.Add("SetParamVec3", lSetParamVec3);
	fmap.Add("SetParamVec4", lSetParamVec4);
	fmap.Add("SetData", lSetData);
	fmap.Add("SetBlendMode", lSetBlendMode);
	fmap.Add("SetPrimitiveType", lSetPrimitiveType);
	fmap.Add("SetOpaque", lSetOpaque);

	constMap.Add("BLEND_ADD",  (int)MaterialBlendMode::Additive);
	constMap.Add("BLEND_MULT", (int)MaterialBlendMode::Multiply);
	constMap.Add("BLEND_NORM", (int)MaterialBlendMode::Normal);
	constMap.Add("PRIM_TRILIST", (int)PrimitiveType::TriangleList);
	constMap.Add("PRIM_TRIFAN", (int)PrimitiveType::TriangleFan);
	constMap.Add("PRIM_TRISTRIP", (int)PrimitiveType::TriangleStrip);
	constMap.Add("PRIM_LINELIST", (int)PrimitiveType::LineList);
	constMap.Add("PRIM_LINESTRIP", (int)PrimitiveType::LineStrip);
	constMap.Add("PRIM_POINTLIST", (int)PrimitiveType::PointList);
	

	auto function = fmap.find(fname);

	if (function != fmap.end())
	{
		lua_pushcfunction(L, function->second);
		return 1;
	}

	auto constValue = constMap.find(fname);

	if (constValue != constMap.end())
	{
		lua_pushinteger(L, constValue->second);
		return 1;
	}
		

	return luaL_error(L, *Utility::Sprintf("ShadedMesh has no: '%s'", *fname));
}

int __gc(lua_State* L) {
	ShadedMesh* object = *static_cast<ShadedMesh**>(lua_touserdata(L, 1));
	delete object;
	return 0;
}

int ShadedMesh::lNew(lua_State* L)
{
	if (lua_gettop(L) == 2)
	{
		ShadedMesh** place = (ShadedMesh**)lua_newuserdata(L, sizeof(ShadedMesh*));
		*place = new ShadedMesh(luaL_checkstring(L, 1), luaL_checkstring(L, 2));
	}
	else if (lua_gettop(L) == 1)
	{
		ShadedMesh** place = (ShadedMesh**)lua_newuserdata(L, sizeof(ShadedMesh*));
		*place = new ShadedMesh(luaL_checkstring(L, 1));
	}
	else {
		ShadedMesh** place = (ShadedMesh**)lua_newuserdata(L, sizeof(ShadedMesh*));
		*place = new ShadedMesh();
	}

	lua_newtable(L);

	lua_pushcfunction(L, __index);
	lua_setfield(L, -2, "__index");
	lua_pushcfunction(L, __gc);
	lua_setfield(L, -2, "__gc");

	lua_setmetatable(L, -2);
	return 1;

}
