/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2001-2002 by NaN Holding BV.
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s): none yet.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file KX_Mesh.h
 *  \ingroup ketsji
 */

#ifndef __KX_MESH_H__
#define __KX_MESH_H__

#include "RAS_Mesh.h"

#include "EXP_Value.h"

class KX_Mesh;
class KX_Scene;
class KX_BlenderMaterial;

#ifdef WITH_PYTHON
// utility conversion function
bool ConvertPythonToMesh(KX_Scene *scene, PyObject *value, KX_Mesh **object, bool py_none_ok, const char *error_prefix);

#endif  // WITH_PYTHON

class KX_Mesh : public EXP_Value, public RAS_Mesh
{
	Py_Header(KX_Mesh)

private:
	KX_Scene *m_scene;

public:
	KX_Mesh(KX_Scene *scene, Mesh *mesh, const LayersInfo& layersInfo);
	KX_Mesh(const KX_Mesh& other);
	virtual ~KX_Mesh();

	// stuff for cvalue related things
	virtual std::string GetName() const;

	void ReplaceScene(KX_Scene *scene);

#ifdef WITH_PYTHON

	EXP_PYMETHOD(KX_Mesh, GetMaterialName);
	EXP_PYMETHOD(KX_Mesh, GetTextureName);

	// both take materialid (int)
	EXP_PYMETHOD(KX_Mesh, GetVertexArrayLength);
	EXP_PYMETHOD(KX_Mesh, GetVertex);
	EXP_PYMETHOD(KX_Mesh, GetPolygon);
	EXP_PYMETHOD(KX_Mesh, Transform);
	EXP_PYMETHOD(KX_Mesh, TransformUV);
	EXP_PYMETHOD(KX_Mesh, ReplaceMaterial);
	EXP_PYMETHOD_NOARGS(KX_Mesh, Copy);

	std::vector<KX_BlenderMaterial *> pyattr_get_materials();
	int pyattr_get_numMaterials();
	int pyattr_get_numPolygons();
	EXP_BaseListWrapper *pyattr_get_polygons();

	unsigned int py_get_polygons_size();
	PyObject *py_get_polygons_item(unsigned int index);

#endif  // WITH_PYTHON
};

#endif  // __KX_MESH_H__
