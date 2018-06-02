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
 * Contributor(s): Mitchell Stokes
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file KX_LibLoadStatus.h
 *  \ingroup bgeconv
 */

#ifndef __KX_LIBLOADSTATUS_H__
#define __KX_LIBLOADSTATUS_H__

#include "EXP_PyObjectPlus.h"
#include "BL_SceneConverter.h"

class BL_Converter;
class KX_KetsjiEngine;
class KX_Scene;
struct Scene;

class KX_LibLoadStatus : public EXP_PyObjectPlus
{
	Py_Header(KX_LibLoadStatus)
private:
	BL_Converter *m_converter;
	KX_KetsjiEngine *m_engine;
	KX_Scene *m_mergescene;
	std::vector<Scene *> m_blenderScenes;
	std::vector<BL_SceneConverter> m_sceneConvertes;
	std::string m_libname;

	float m_progress;
	double m_starttime;
	double m_endtime;

	/// The current status of this libload, used by the scene converter.
	bool m_finished;

#ifdef WITH_PYTHON
	PyObject *m_finish_cb;
	PyObject *m_progress_cb;
#endif

public:
	KX_LibLoadStatus(BL_Converter *converter, KX_KetsjiEngine *engine, KX_Scene *merge_scene, const std::string& path);

	/// Called when the libload is done.
	void Finish();
	void RunFinishCallback();
	void RunProgressCallback();

	BL_Converter *GetConverter() const;
	KX_KetsjiEngine *GetEngine() const;
	KX_Scene *GetMergeScene() const;

	const std::vector<Scene *>& GetBlenderScenes() const;
	void SetBlenderScenes(const std::vector<Scene *>& scenes);
	std::vector<BL_SceneConverter>& GetSceneConverters();
	void AddSceneConverter(BL_SceneConverter&& converter);

	bool IsFinished() const;

	void SetProgress(float progress);
	float GetProgress() const;
	void AddProgress(float progress);

#ifdef WITH_PYTHON
	PyObject *pyattr_get_onfinish();
	bool pyattr_set_onfinish(PyObject *value);
	PyObject *pyattr_get_onprogress();
	bool pyattr_set_onprogress(PyObject *value);

	float pyattr_get_timetaken();
#endif
};

#endif  // __KX_LIBLOADSTATUS_H__
