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
 * Ketsji scene. Holds references to all scene data.
 */

/** \file gameengine/Ketsji/KX_Scene.cpp
 *  \ingroup ketsji
 */


#ifdef _MSC_VER
#  pragma warning (disable:4786)
#endif

#include "KX_Scene.h"
#include "KX_Globals.h"
#include "BLI_utildefines.h"
#include "KX_KetsjiEngine.h"
#include "KX_BlenderMaterial.h"
#include "KX_TextMaterial.h"
#include "KX_FontObject.h"
#include "RAS_IPolygonMaterial.h"
#include "KX_Camera.h"
#include "KX_PyMath.h"
#include "KX_Mesh.h"
#include "KX_Scene.h"
#include "KX_LodManager.h"
#include "KX_CullingHandler.h"

#include "RAS_Rasterizer.h"
#include "RAS_ICanvas.h"
#include "RAS_2DFilterData.h"
#include "KX_2DFilterManager.h"
#include "RAS_BoundingBoxManager.h"
#include "RAS_BucketManager.h"
#include "RAS_Deformer.h"

#include "EXP_PropFloat.h"
#include "SG_Node.h"
#include "SG_Controller.h"
#include "SG_Node.h"
#include "DNA_group_types.h"
#include "DNA_scene_types.h"
#include "DNA_property_types.h"

#include "KX_NodeRelationships.h"

#include "KX_NetworkMessageScene.h"
#include "PHY_IPhysicsEnvironment.h"
#include "PHY_IGraphicController.h"
#include "PHY_IPhysicsController.h"
#include "BL_Converter.h"
#include "KX_MotionState.h"

#include "BL_DeformableGameObject.h"
#include "KX_ObstacleSimulation.h"

#ifdef WITH_BULLET
#  include "KX_SoftBodyDeformer.h"
#endif

#ifdef WITH_PYTHON
#  include "EXP_PythonCallBack.h"
#  include "Texture.h" // For FreeAllTextures.
#endif  // WITH_PYTHON

#include "KX_LightObject.h"

#include "BLI_task.h"

#include "BKE_library.h" // For IS_TAGGED

#include "CM_Message.h"
#include "CM_List.h"

static void *KX_SceneReplicationFunc(SG_Node *node, void *gameobj, void *scene)
{
	KX_GameObject *replica = ((KX_Scene *)scene)->AddNodeReplicaObject(node, (KX_GameObject *)gameobj);
	return (void *)replica;
}

static void *KX_SceneDestructionFunc(SG_Node *node, void *gameobj, void *scene)
{
	((KX_Scene *)scene)->RemoveNodeDestructObject((KX_GameObject *)gameobj);

	return nullptr;
}

bool KX_Scene::KX_ScenegraphUpdateFunc(SG_Node *node, void *gameobj, void *scene)
{
	return node->Schedule(((KX_Scene *)scene)->m_sghead);
}

bool KX_Scene::KX_ScenegraphRescheduleFunc(SG_Node *node, void *gameobj, void *scene)
{
	return node->Reschedule(((KX_Scene *)scene)->m_sghead);
}

SG_Callbacks KX_Scene::m_callbacks = SG_Callbacks(
	KX_SceneReplicationFunc,
	KX_SceneDestructionFunc,
	KX_GameObject::UpdateTransformFunc,
	KX_Scene::KX_ScenegraphUpdateFunc,
	KX_Scene::KX_ScenegraphRescheduleFunc);

KX_Scene::KX_Scene(SCA_IInputDevice *inputDevice,
                   const std::string& sceneName,
                   Scene *scene,
                   RAS_ICanvas *canvas,
                   KX_NetworkMessageManager *messageManager) :
	m_physicsEnvironment(0),
	m_name(sceneName),
	m_activeCamera(nullptr),
	m_overrideCullingCamera(nullptr),
	m_suspend(false),
	m_suspendedDelta(0.0),
	m_activityCulling(false),
	m_dbvtCulling(false),
	m_dbvtOcclusionRes(0),
	m_blenderScene(scene),
	m_previousAnimTime(0.0f),
	m_isActivedHysteresis(false),
	m_lodHysteresisValue(0)
{
	m_filterManager = new KX_2DFilterManager();

	m_networkScene = new KX_NetworkMessageScene(messageManager);

	m_rendererManager = new KX_TextureRendererManager(this);
	KX_TextMaterial *textMaterial = new KX_TextMaterial();
	m_bucketmanager = new RAS_BucketManager(textMaterial);
	m_boundingBoxManager = new RAS_BoundingBoxManager();

	m_animationPool = BLI_task_pool_create(KX_GetActiveEngine()->GetTaskScheduler(), &m_animationPoolData);

#ifdef WITH_PYTHON
	m_attrDict = nullptr;

	for (unsigned short i = 0; i < MAX_DRAW_CALLBACK; ++i) {
		m_drawCallbacks[i] = nullptr;
	}
#endif
}

KX_Scene::~KX_Scene()
{
#ifdef WITH_PYTHON
	Texture::FreeAllTextures(this);
#endif  // WITH_PYTHON

	/* The release of debug properties used to be in KX_Scene::~KX_Scene
	 * It's still there but we remove all properties here otherwise some
	 * reference might be hanging and causing late release of objects
	 */
	RemoveAllDebugProperties();

	while (!m_parentlist.Empty()) {
		KX_GameObject *parentobj = m_parentlist.GetFront();
		RemoveObject(parentobj);
	}

	// Free all ressources.
	m_resssources.Clear();

	if (m_obstacleSimulation) {
		delete m_obstacleSimulation;
	}

	if (m_animationPool) {
		BLI_task_pool_free(m_animationPool);
	}

	if (m_filterManager) {
		delete m_filterManager;
	}

	if (m_physicsEnvironment) {
		delete m_physicsEnvironment;
	}

	if (m_networkScene) {
		delete m_networkScene;
	}

	if (m_rendererManager) {
		delete m_rendererManager;
	}

	if (m_bucketmanager) {
		delete m_bucketmanager;
	}

	if (m_boundingBoxManager) {
		delete m_boundingBoxManager;
	}

#ifdef WITH_PYTHON
	if (m_attrDict) {
		PyDict_Clear(m_attrDict);
		Py_CLEAR(m_attrDict);
	}

	// These may be nullptr but the macro checks.
	for (unsigned short i = 0; i < MAX_DRAW_CALLBACK; ++i) {
		Py_CLEAR(m_drawCallbacks[i]);
	}
#endif
}

BL_ResourceCollection& KX_Scene::GetResources()
{
	return m_resssources;
}

void KX_Scene::SetResources(BL_ResourceCollection&& ressources)
{
	m_resssources = std::move(ressources);
}

std::string KX_Scene::GetName() const
{
	return m_name;
}

void KX_Scene::SetName(const std::string& name)
{
	m_name = name;
}

RAS_BucketManager *KX_Scene::GetBucketManager() const
{
	return m_bucketmanager;
}

KX_TextureRendererManager *KX_Scene::GetTextureRendererManager() const
{
	return m_rendererManager;
}

RAS_BoundingBoxManager *KX_Scene::GetBoundingBoxManager() const
{
	return m_boundingBoxManager;
}

EXP_ListValue<KX_GameObject>& KX_Scene::GetObjectList()
{
	return m_objectlist;
}

EXP_ListValue<KX_GameObject>& KX_Scene::GetRootParentList()
{
	return m_parentlist;
}

EXP_ListValue<KX_GameObject>& KX_Scene::GetInactiveList()
{
	return m_inactivelist;
}

EXP_ListValue<KX_LightObject>& KX_Scene::GetLightList()
{
	return m_lightlist;
}

EXP_ListValue<KX_Camera>& KX_Scene::GetCameraList()
{
	return m_cameralist;
}

EXP_ListValue<KX_FontObject>& KX_Scene::GetFontList()
{
	return m_fontlist;
}

KX_PythonComponentManager& KX_Scene::GetPythonComponentManager()
{
	return m_componentManager;
}

void KX_Scene::SetFramingType(const RAS_FrameSettings& frameSettings)
{
	m_frameSettings = frameSettings;
}

const RAS_FrameSettings& KX_Scene::GetFramingType() const
{
	return m_frameSettings;
}

void KX_Scene::SetWorldInfo(KX_WorldInfo *worldinfo)
{
	m_worldinfo.reset(worldinfo);
}

KX_WorldInfo *KX_Scene::GetWorldInfo() const
{
	return m_worldinfo.get();
}

void KX_Scene::Suspend()
{
	m_suspend = true;
}

void KX_Scene::Resume()
{
	m_suspend = false;
}

void KX_Scene::SetActivityCulling(bool b)
{
	m_activityCulling = b;
}

bool KX_Scene::IsSuspended() const
{
	return m_suspend;
}

void KX_Scene::SetDbvtCulling(bool b)
{
	m_dbvtCulling = b;
}

bool KX_Scene::GetDbvtCulling() const
{
	return m_dbvtCulling;
}

void KX_Scene::SetDbvtOcclusionRes(int i)
{
	m_dbvtOcclusionRes = i;
}

int KX_Scene::GetDbvtOcclusionRes() const
{
	return m_dbvtOcclusionRes;
}

void KX_Scene::AddObjectDebugProperties(KX_GameObject *gameobj)
{
#if 0
	Object *blenderobject = gameobj->GetBlenderObject();
	if (!blenderobject) {
		return;
	}

	for (bProperty *prop = (bProperty *)blenderobject->prop.first; prop; prop = prop->next) {
		if (prop->flag & PROP_DEBUG) {
			AddDebugProperty(gameobj, prop->name);
		}
	}

	if (blenderobject->scaflag & OB_DEBUGSTATE) {
		AddDebugProperty(gameobj, "__state__");
	}
#endif // TODO add properties in BL_ConverterObjectInfo.
}

void KX_Scene::RemoveNodeDestructObject(KX_GameObject *gameobj)
{
	NewRemoveObject(gameobj);
}

KX_GameObject *KX_Scene::AddNodeReplicaObject(SG_Node *node, KX_GameObject *gameobj)
{
	/* For group duplication, limit the duplication of the hierarchy to the
	 * objects that are part of the group. */
	if (!IsObjectInGroup(gameobj)) {
		return nullptr;
	}

	KX_GameObject *newobj = static_cast<KX_GameObject *>(gameobj->GetReplica());

	// Add properties to debug list, for added objects and DupliGroups.
	if (KX_GetActiveEngine()->GetFlag(KX_KetsjiEngine::AUTO_ADD_DEBUG_PROPERTIES)) {
		AddObjectDebugProperties(newobj);
	}

	// Also register 'timers' (time properties) of the replica.
	for (unsigned short i = 0, numprops = newobj->GetPropertyCount(); i < numprops; ++i) {
		EXP_PropValue *prop = newobj->GetProperty(i);

		/*if (prop->GetProperty("timer")) {
// 			m_timemgr->AddTimeProperty(prop); TODO
		}*/
	}

	if (node) {
		newobj->SetNode(node);
	}
	else {
		SG_Node *rootnode = new SG_Node(newobj, this, KX_Scene::m_callbacks);

		// This fixes part of the scaling-added object bug.
		SG_Node *orgnode = gameobj->GetNode();
		rootnode->SetLocalScale(orgnode->GetLocalScale());
		rootnode->SetLocalPosition(orgnode->GetLocalPosition());
		rootnode->SetLocalOrientation(orgnode->GetLocalOrientation());

		// Define the relationship between this node and it's parent.
		KX_NormalParentRelation *parent_relation = new KX_NormalParentRelation();
		rootnode->SetParentRelation(parent_relation);

		newobj->SetNode(rootnode);
	}

	SG_Node *replicanode = newobj->GetNode();

	// Add the object in the obstacle simulation if needed.
	/*if (m_obstacleSimulation && gameobj->GetBlenderObject()->gameflag & OB_HASOBSTACLE) {
		m_obstacleSimulation->AddObstacleForObj(newobj);
	} TODO BL_ConverterObjectInfo */ 

	// Register object for component update.
	if (gameobj->GetComponents()) {
		m_componentManager.RegisterObject(newobj);
	}

	replicanode->SetClientObject(newobj);

	// This is the list of object that are send to the graphics pipeline.
	m_objectlist.Add(newobj);

	switch (newobj->GetObjectType()) {
		case KX_GameObject::OBJECT_TYPE_LIGHT:
		{
			m_lightlist.Add(static_cast<KX_LightObject *>(newobj));
			break;
		}
		case KX_GameObject::OBJECT_TYPE_TEXT:
		{
			m_fontlist.Add(static_cast<KX_FontObject *>(newobj));
			break;
		}
		case KX_GameObject::OBJECT_TYPE_CAMERA:
		{
			m_cameralist.Add(static_cast<KX_Camera *>(newobj));
			break;
		}
		case KX_GameObject::OBJECT_TYPE_ARMATURE:
		{
			AddAnimatedObject(newobj);
			break;
		}
		default:
		{
			break;
		}
	}
	newobj->AddMeshUser();

	// Logic cannot be replicated, until the whole hierarchy is replicated.
	m_logicHierarchicalGameObjects.push_back(newobj);

	// Replicate graphic controller.
	if (gameobj->GetGraphicController()) {
		PHY_IMotionState *motionstate = new KX_MotionState(newobj->GetNode());
		PHY_IGraphicController *newctrl = gameobj->GetGraphicController()->GetReplica(motionstate);
		newctrl->SetNewClientInfo(&newobj->GetClientInfo());
		newobj->SetGraphicController(newctrl);
	}

	// Replicate physics controller.
	if (gameobj->GetPhysicsController()) {
		PHY_IMotionState *motionstate = new KX_MotionState(newobj->GetNode());
		PHY_IPhysicsController *newctrl = gameobj->GetPhysicsController()->GetReplica();

		KX_GameObject *parent = newobj->GetParent();
		PHY_IPhysicsController *parentctrl = (parent) ? parent->GetPhysicsController() : nullptr;

		newctrl->SetNewClientInfo(&newobj->GetClientInfo());
		newobj->SetPhysicsController(newctrl);
		newctrl->PostProcessReplica(motionstate, parentctrl);

		// Child objects must be static.
		if (parent) {
			newctrl->SuspendDynamics();
		}
	}

	// Always make sure that the bounding box is valid.
	newobj->UpdateBounds(true);

	return newobj;
}

void KX_Scene::DupliGroupRecurse(KX_GameObject *groupobj, int level)
{
#if 0
	Object *blgroupobj = groupobj->GetBlenderObject();
	std::vector<KX_GameObject *> duplilist;

	if (!groupobj->GetNode() || !groupobj->IsDupliGroup() || level > MAX_DUPLI_RECUR) {
		return;
	}

	// We will add one group at a time.
	m_logicHierarchicalGameObjects.clear();

	/* For groups will do something special:
	 * we will force the creation of objects to those in the group only
	 * Again, this is match what Blender is doing (it doesn't care of parent relationship)
	 */
	m_groupGameObjects.clear();

	Group *group = blgroupobj->dup_group;
	for (GroupObject *go = (GroupObject *)group->gobject.first; go; go = (GroupObject *)go->next) {
		Object *blenderobj = go->ob;
		if (blgroupobj == blenderobj) {
			// This check is also in group_duplilist().
			continue;
		}

		// TODO put information about hierarchy in BL_ConverterObjectInfo.
		Put KX_GameObject pointers into BL_ConvertObjectInfo and check for existance into m_objectlist and m_inactivelist.
#if 0
		KX_GameObject *gameobj = (KX_GameObject *)m_logicmgr->FindGameObjByBlendObj(blenderobj);
		if (gameobj == nullptr) {
			/* This object has not been converted.
			 * Should not happen as dupli group are created automatically */
			continue;
		}

		if ((blenderobj->lay & group->layer) == 0) {
			// Object is not visible in the 3D view, will not be instantiated.
			continue;
		}
		m_groupGameObjects.insert(gameobj);
#endif
	}

	for (KX_GameObject *gameobj : m_groupGameObjects) {
		KX_GameObject *parent = gameobj->GetParent();
		if (parent != nullptr) {
			/* This object is not a top parent. Either it is the child of another
			 * object in the group and it will be added automatically when the parent
			 * is added. Or it is the child of an object outside the group and the group
			 * is inconsistent, skip it anyway.
			 */
			continue;
		}
		KX_GameObject *replica = AddNodeReplicaObject(nullptr, gameobj);
		// Add to 'rootparent' list (this is the list of top hierarchy objects, updated each frame).
		m_parentlist.Add(replica);

		/* Set references for dupli-group
		 * groupobj holds a list of all objects, that belongs to this group. */
		groupobj->AddInstanceObjects(replica);

		// Every object gets the reference to its dupli-group object.
		replica->SetDupliGroupObject(groupobj);

		// Recurse replication into children nodes.
		const NodeList& children = gameobj->GetNode()->GetChildren();

		replica->GetNode()->ClearSGChildren();
		for (SG_Node *orgnode : children) {
			SG_Node *childreplicanode = orgnode->GetReplica();
			if (childreplicanode) {
				replica->GetNode()->AddChild(childreplicanode);
			}
		}
		/* Don't replicate logic now: we assume that the objects in the group can have
		 * logic relationship, even outside parent relationship
		 * In order to match 3D view, the position of groupobj is used as a
		 * transformation matrix instead of the new position. This means that
		 * the group reference point is 0,0,0.
		 */

		// Get the rootnode's scale.
		const mt::vec3& newscale = groupobj->NodeGetWorldScaling();
		// Set the replica's relative scale with the rootnode's scale.
		replica->NodeSetRelativeScale(newscale);

		const mt::vec3 offset(group->dupli_ofs);
		const mt::vec3 newpos = groupobj->NodeGetWorldPosition() +
		                        newscale * (groupobj->NodeGetWorldOrientation() * (gameobj->NodeGetWorldPosition() - offset));
		replica->NodeSetLocalPosition(newpos);
		// Set the orientation after position for softbody.
		const mt::mat3 newori = groupobj->NodeGetWorldOrientation() * gameobj->NodeGetWorldOrientation();
		replica->NodeSetLocalOrientation(newori);
		// Update scenegraph for entire tree of children.
		replica->GetNode()->UpdateWorldData();
		// We can now add the graphic controller to the physic engine.
		replica->ActivateGraphicController(true);

		// Done with replica.
		replica->Release();
	}

	// Relink any pointers as necessary, sort of a temporary solution.
	for (KX_GameObject *gameobj : m_logicHierarchicalGameObjects) {
		// Add the object in the layer of the parent.
		gameobj->SetLayer(groupobj->GetLayer());
	}

	// Now look if object in the hierarchy have dupli group and recurse.
	for (KX_GameObject *gameobj : m_logicHierarchicalGameObjects) {
		// Replicate all constraints.
		gameobj->ReplicateConstraints(m_physicsEnvironment, m_logicHierarchicalGameObjects);

		if (gameobj != groupobj && gameobj->IsDupliGroup()) {
			// Can't instantiate group immediately as it destroys m_logicHierarchicalGameObjects.
			duplilist.push_back(gameobj);
		}
	}

	for (KX_GameObject *gameobj : duplilist) {
		DupliGroupRecurse(gameobj, level + 1);
	}
#endif // TODO BL_ConverterObjectInfo */
}

bool KX_Scene::IsObjectInGroup(KX_GameObject *gameobj) const
{
	return (m_groupGameObjects.empty() || m_groupGameObjects.find(gameobj) != m_groupGameObjects.end());
}

KX_GameObject *KX_Scene::AddReplicaObject(KX_GameObject *originalobj, KX_GameObject *referenceobj, float lifespan)
{
	m_logicHierarchicalGameObjects.clear();
	m_groupGameObjects.clear();

	// Lets create a replica.
	KX_GameObject *replica = AddNodeReplicaObject(nullptr, originalobj);

	/* Add a timebomb to this object
	 * lifespan of zero means 'this object lives forever'. */
	if (lifespan > 0.0f) {
		// For now, convert between so called frames and realtime.
		m_tempObjectList.push_back(replica);
		/* This convert the life from frames to sort-of seconds, hard coded 0.02 that assumes we have 50 frames per second
		 * if you change this value, make sure you change it in KX_GameObject::pyattr_get_life property too. */
		EXP_PropValue *fval = new EXP_PropFloat(lifespan * 0.02f);
		replica->SetProperty("::timebomb", fval);
	}

	// Add to 'rootparent' list (this is the list of top hierarchy objects, updated each frame).
	m_parentlist.Add(replica);

	// Recurse replication into children nodes.

	const NodeList& children = originalobj->GetNode()->GetChildren();

	replica->GetNode()->ClearSGChildren();
	for (SG_Node *orgnode : children) {
		SG_Node *childreplicanode = orgnode->GetReplica();
		if (childreplicanode) {
			replica->GetNode()->AddChild(childreplicanode);
		}
	}

	if (referenceobj) {
		/* At this stage all the objects in the hierarchy have been duplicated,
		 * we can update the scenegraph, we need it for the duplication of logic. */
		const mt::vec3& newpos = referenceobj->NodeGetWorldPosition();
		replica->NodeSetLocalPosition(newpos);

		const mt::mat3& newori = referenceobj->NodeGetWorldOrientation();
		replica->NodeSetLocalOrientation(newori);

		// Get the rootnode's scale.
		const mt::vec3& newscale = referenceobj->GetNode()->GetRootSGParent()->GetLocalScale();
		// Set the replica's relative scale with the rootnode's scale.
		replica->NodeSetRelativeScale(newscale);
	}

	replica->GetNode()->UpdateWorldData();
	// The size is correct, we can add the graphic controller to the physic engine.
	replica->ActivateGraphicController(true);

	// Relink any pointers as necessary, sort of a temporary solution.
	for (KX_GameObject *gameobj : m_logicHierarchicalGameObjects) {
		if (referenceobj) {
			// Add the object in the layer of the reference object.
			gameobj->SetLayer(referenceobj->GetLayer());
		}
		else {
			// We don't know what layer set, so we set all visible layers in the blender scene.
			gameobj->SetLayer(m_blenderScene->lay);
		}
	}

	// Check if there are objects with dupligroup in the hierarchy.
	std::vector<KX_GameObject *> duplilist;
	for (KX_GameObject *gameobj : m_logicHierarchicalGameObjects) {
		if (gameobj->IsDupliGroup()) {
			// Separate list as m_logicHierarchicalGameObjects is also used by DupliGroupRecurse().
			duplilist.push_back(gameobj);
		}
	}
	for (KX_GameObject *gameobj : duplilist) {
		DupliGroupRecurse(gameobj, 0);
	}

	// Don't release replica here because we are returning it, not done with it...
	return replica;
}

void KX_Scene::RemoveObject(KX_GameObject *gameobj)
{
	// Disconnect child from parent.
	SG_Node *node = gameobj->GetNode();

	if (node) {
		node->DisconnectFromParent();

		// Recursively destruct.
		node->Destruct();
	}
}

void KX_Scene::RemoveDupliGroup(KX_GameObject *gameobj)
{
	if (gameobj->IsDupliGroup()) {
		for (KX_GameObject *instance : gameobj->GetInstanceObjects()) {
			DelayedRemoveObject(instance);
		}
	}
}

void KX_Scene::DelayedRemoveObject(KX_GameObject *gameobj)
{
	RemoveDupliGroup(gameobj);

	CM_ListAddIfNotFound(m_euthanasyobjects, gameobj);
}

void KX_Scene::NewRemoveObject(KX_GameObject *gameobj)
{
	// Remove property from debug list.
	RemoveObjectDebugProperties(gameobj);

	/* Invalidate the python reference, since the object may exist in script lists
	 * its possible that it wont be automatically invalidated, so do it manually here,
	 *
	 * if for some reason the object is added back into the scene python can always get a new Proxy
	 */
	gameobj->InvalidateProxy();

	// Now remove the timer properties from the time manager.
	for (unsigned short i = 0, numprops = gameobj->GetPropertyCount(); i < numprops; ++i) {
		EXP_PropValue *propval = gameobj->GetProperty(i);
		/*if (propval->GetProperty("timer")) {
// 			m_timemgr->RemoveTimeProperty(propval);
		}*/
	}

	/* If the object is the dupligroup proxy, you have to cleanup all m_dupliGroupObject's in all
	 * instances refering to this group. */
	if (gameobj->GetInstanceObjects()) {
		for (KX_GameObject *instance : gameobj->GetInstanceObjects()) {
			instance->RemoveDupliGroupObject();
		}
	}

	// If this object was part of a group, make sure to remove it from that group's instance list.
	KX_GameObject *group = gameobj->GetDupliGroupObject();
	if (group) {
		group->RemoveInstanceObject(gameobj);
	}

	if (m_obstacleSimulation) {
		m_obstacleSimulation->DestroyObstacleForObj(gameobj);
	}

	m_componentManager.UnregisterObject(gameobj);
	m_rendererManager->InvalidateViewpoint(gameobj);

	switch (gameobj->GetObjectType()) {
		case KX_GameObject::OBJECT_TYPE_CAMERA:
		{
			m_cameralist.RemoveValue(static_cast<KX_Camera *>(gameobj));
			break;
		}
		case KX_GameObject::OBJECT_TYPE_LIGHT:
		{
			m_lightlist.RemoveValue(static_cast<KX_LightObject *>(gameobj));
			break;
		}
		case KX_GameObject::OBJECT_TYPE_TEXT:
		{
			m_fontlist.RemoveValue(static_cast<KX_FontObject *>(gameobj));
			break;
		}
		default:
		{
			break;
		}
	}

	CM_ListRemoveIfFound(m_animatedlist, gameobj);
	CM_ListRemoveIfFound(m_euthanasyobjects, gameobj);
	CM_ListRemoveIfFound(m_tempObjectList, gameobj);

	m_parentlist.RemoveValue(gameobj);
	m_inactivelist.RemoveValue(gameobj);
	m_objectlist.RemoveValue(gameobj);

	if (gameobj == m_activeCamera) {
		m_activeCamera = nullptr;
	}

	if (gameobj == m_overrideCullingCamera) {
		m_overrideCullingCamera = nullptr;
	}

	delete gameobj;
}

KX_Camera *KX_Scene::GetActiveCamera()
{
	// nullptr if not defined.
	return m_activeCamera;
}

void KX_Scene::SetActiveCamera(KX_Camera *cam)
{
	m_activeCamera = cam;
}

KX_Camera *KX_Scene::GetOverrideCullingCamera() const
{
	return m_overrideCullingCamera;
}

void KX_Scene::SetOverrideCullingCamera(KX_Camera *cam)
{
	m_overrideCullingCamera = cam;
}

void KX_Scene::SetCameraOnTop(KX_Camera *cam)
{
	// Change camera place.
	m_cameralist.RemoveValue(cam);
	m_cameralist.Add(cam);
}

void KX_Scene::PhysicsCullingCallback(KX_ClientObjectInfo *objectInfo, void *cullingInfo)
{
	CullingInfo *info = static_cast<CullingInfo *>(cullingInfo);
	KX_GameObject *gameobj = objectInfo->m_gameobject;
	if (!gameobj->GetVisible() || !gameobj->UseCulling()) {
		// Ideally, invisible objects should be removed from the culling tree temporarily.
		return;
	}
	if (info->m_layer && !(gameobj->GetLayer() & info->m_layer)) {
		// Used for shadow: object is not in shadow layer.
		return;
	}

	// Make object visible.
	gameobj->SetCulled(false);
	info->m_objects.push_back(gameobj);
}

void KX_Scene::CalculateVisibleMeshes(std::vector<KX_GameObject *>& objects, KX_Camera *cam, int layer)
{
	if (!cam->GetFrustumCulling()) {
		for (KX_GameObject *gameobj : m_objectlist) {
			gameobj->GetCullingNode()->SetCulled(false);
			objects.push_back(gameobj);
		}
		return;
	}

	CalculateVisibleMeshes(objects, cam->GetFrustum(), layer);
}

void KX_Scene::CalculateVisibleMeshes(std::vector<KX_GameObject *>& objects, const SG_Frustum& frustum, int layer)
{
	m_boundingBoxManager->Update(false);

	bool dbvt_culling = false;
	if (m_dbvtCulling) {
		for (KX_GameObject *gameobj : m_objectlist) {
			gameobj->SetCulled(true);
			/* Reset KX_GameObject m_culled to true before doing culling
			 * since DBVT culling will only set it to false.
			 */
			if (gameobj->GetDeformer()) {
				/** Update all the deformer, not only per material.
				 * One of the side effect is to clear some flags about AABB calculation.
				 * like in KX_SoftBodyDeformer.
				 */
				gameobj->GetDeformer()->UpdateBuckets();
			}
			// Update the object bounding volume box.
			gameobj->UpdateBounds(false);
		}

		// Test culling through Bullet, get the clip planes.
		const std::array<mt::vec4, 6>& planes = frustum.GetPlanes();
		const mt::mat4& matrix = frustum.GetMatrix();
		const int *viewport = KX_GetActiveEngine()->GetCanvas()->GetViewPort();
		CullingInfo info(layer, objects);

		dbvt_culling = m_physicsEnvironment->CullingTest(PhysicsCullingCallback, &info, planes, m_dbvtOcclusionRes, viewport, matrix);
	}
	if (!dbvt_culling) {
		KX_CullingHandler handler(objects, frustum);
		for (KX_GameObject *gameobj : m_objectlist) {
			if (gameobj->UseCulling() && gameobj->GetVisible() && (layer == 0 || gameobj->GetLayer() & layer)) {
				if (gameobj->GetDeformer()) {
					/** Update all the deformer, not only per material.
					 * One of the side effect is to clear some flags about AABB calculation.
					 * like in KX_SoftBodyDeformer.
					 */
					gameobj->GetDeformer()->UpdateBuckets();
				}
				// Update the object bounding volume box.
				gameobj->UpdateBounds(false);

				handler.Process(gameobj);
			}
		}
	}

	m_boundingBoxManager->ClearModified();
}

void KX_Scene::DrawDebug(RAS_DebugDraw& debugDraw, const std::vector<KX_GameObject *>& objects,
                         KX_DebugOption showBoundingBox, KX_DebugOption showArmatures)
{
	if (showBoundingBox != KX_DebugOption::DISABLE) {
		for (KX_GameObject *gameobj : objects) {
			const mt::vec3& scale = gameobj->NodeGetWorldScaling();
			const mt::vec3& position = gameobj->NodeGetWorldPosition();
			const mt::mat3& orientation = gameobj->NodeGetWorldOrientation();
			const SG_BBox& box = gameobj->GetCullingNode()->GetAabb();
			const mt::vec3& center = box.GetCenter();

			debugDraw.DrawAabb(position, orientation, box.GetMin() * scale, box.GetMax() * scale,
			                   mt::vec4(1.0f, 0.0f, 1.0f, 1.0f));

			// Render center in red, green and blue.
			debugDraw.DrawLine(orientation * (center * scale) + position,
			                   orientation * ((center + mt::axisX3) * scale) + position,
			                   mt::vec4(1.0f, 0.0f, 0.0f, 1.0f));
			debugDraw.DrawLine(orientation * (center * scale) + position,
			                   orientation * ((center + mt::axisY3) * scale)  + position,
			                   mt::vec4(0.0f, 1.0f, 0.0f, 1.0f));
			debugDraw.DrawLine(orientation * (center * scale) + position,
			                   orientation * ((center + mt::axisZ3) * scale)  + position,
			                   mt::vec4(0.0f, 0.0f, 1.0f, 1.0f));
		}
	}

	if (showArmatures != KX_DebugOption::DISABLE) {
		// The side effect of a armature is that it was added in the animated object list.
		/*for (KX_GameObject *gameobj : m_animatedlist) {
			if (gameobj->GetObjectType() == KX_GameObject::OBJECT_TYPE_ARMATURE) {
				BL_ArmatureObject *armature = static_cast<BL_ArmatureObject *>(gameobj);
				if (showArmatures == KX_DebugOption::FORCE || armature->GetDrawDebug()) {
					armature->DrawDebug(debugDraw);
				}
			}
		}*/
	}
}

void KX_Scene::RenderDebugProperties(RAS_DebugDraw& debugDraw, int xindent, int ysize, int& xcoord, int& ycoord, unsigned short propsMax)
{
	static const mt::vec4 white(1.0f, 1.0f, 1.0f, 1.0f);

	unsigned short numprop = m_debugList.size();
	if (numprop > propsMax) {
		numprop = propsMax;
	}

	for (const DebugProp& debugProp : m_debugList) {
		KX_GameObject *gameobj = debugProp.m_obj;
		const std::string objname = gameobj->GetName();
		const std::string& propname = debugProp.m_name;

		EXP_PropValue *propval = gameobj->GetProperty(propname);
		if (propval) {
			const std::string text = propval->GetText();
			const std::string debugtxt = objname + ": '" + propname + "' = " + text;
			debugDraw.RenderText2d(debugtxt, mt::vec2(xcoord + xindent, ycoord), white);
			ycoord += ysize;
		}
	}
}

void KX_Scene::LogicBeginFrame(double curtime, double framestep)
{
	// Have a look at temp objects.
	for (KX_GameObject *gameobj : m_tempObjectList) {
		EXP_PropFloat *propval = static_cast<EXP_PropFloat *>(gameobj->GetProperty("::timebomb"));

		if (propval) {
			const double timeleft = propval->GetValue() - framestep;

			if (timeleft > 0) {
				propval->SetValue(timeleft);
			}
			else {
				// Remove obj, remove the object from tempObjectList in NewRemoveObject only.
				DelayedRemoveObject(gameobj);
			}
		}
		else {
			// All object is the tempObjectList should have a clock.
			BLI_assert(false);
		}
	}
}

void KX_Scene::AddAnimatedObject(KX_GameObject *gameobj)
{
	CM_ListAddIfNotFound(m_animatedlist, gameobj);
}

bool KX_Scene::PropertyInDebugList(KX_GameObject *gameobj, const std::string &name)
{
	for (const DebugProp& prop : m_debugList) {
		if (prop.m_obj == gameobj && prop.m_name == name) {
			return true;
		}
	}
	return false;
}

bool KX_Scene::ObjectInDebugList(KX_GameObject *gameobj)
{
	for (const DebugProp& prop : m_debugList) {
		if (prop.m_obj == gameobj) {
			return true;
		}
	}
	return false;
}

void KX_Scene::AddDebugProperty(KX_GameObject *gameobj, const std::string &name)
{
	if (m_debugList.size() < 100) {
		m_debugList.push_back({gameobj, name});
	}
}

void KX_Scene::RemoveDebugProperty(KX_GameObject *gameobj, const std::string &name)
{
	for (std::vector<DebugProp>::iterator it = m_debugList.begin(); it != m_debugList.end(); ) {
		const DebugProp& prop = *it;

		if (prop.m_obj == gameobj && prop.m_name == name) {
			it = m_debugList.erase(it);
			break;
		}
		else {
			++it;
		}
	}
}

void KX_Scene::RemoveObjectDebugProperties(KX_GameObject *gameobj)
{
	for (std::vector<DebugProp>::iterator it = m_debugList.begin(); it != m_debugList.end(); ) {
		const DebugProp& prop = *it;

		if (prop.m_obj == gameobj) {
			it = m_debugList.erase(it);
			continue;
		}
		else {
			++it;
		}
	}
}

void KX_Scene::RemoveAllDebugProperties()
{
	m_debugList.clear();
}

static void update_anim_thread_func(TaskPool *pool, void *taskdata, int UNUSED(threadid))
{
	KX_Scene::AnimationPoolData *data = (KX_Scene::AnimationPoolData *)BLI_task_pool_userdata(pool);
	double curtime = data->curtime;

	KX_GameObject *gameobj = (KX_GameObject *)taskdata;

	// Non-armature updates are fast enough, so just update them
	bool needs_update = gameobj->GetObjectType() != KX_GameObject::OBJECT_TYPE_ARMATURE;

	if (!needs_update) {
		// If we got here, we're looking to update an armature, so check its children meshes
		// to see if we need to bother with a more expensive pose update
		const std::vector<KX_GameObject *> children = gameobj->GetChildren();

		bool has_mesh = false, has_non_mesh = false;

		// Check for meshes that haven't been culled
		for (KX_GameObject *child : children) {
			if (!child->GetCulled()) {
				needs_update = true;
				break;
			}

			if (child->GetMeshList().empty()) {
				has_non_mesh = true;
			}
			else {
				has_mesh = true;
			}
		}

		// If we didn't find a non-culled mesh, check to see
		// if we even have any meshes, and update if this
		// armature has only non-mesh children.
		if (!needs_update && !has_mesh && has_non_mesh) {
			needs_update = true;
		}
	}

	// If the object is a culled armature, then we manage only the animation time and end of its animations.
	gameobj->UpdateActionManager(curtime, needs_update);

	if (needs_update) {
		const std::vector<KX_GameObject *> children = gameobj->GetChildren();
		KX_GameObject *parent = gameobj->GetParent();

		// Only do deformers here if they are not parented to an armature, otherwise the armature will
		// handle updating its children
		if (gameobj->GetDeformer() && (!parent || parent->GetObjectType() != KX_GameObject::OBJECT_TYPE_ARMATURE)) {
			gameobj->GetDeformer()->Update();
		}

		for (KX_GameObject *child : children) {
			if (child->GetDeformer()) {
				child->GetDeformer()->Update();
			}
		}
	}
}

void KX_Scene::UpdateAnimations(double curtime, bool restrict)
{
	if (restrict) {
		const double animTimeStep = 1.0 / m_blenderScene->r.frs_sec;

		/* Don't update if the time step is too small and if we are not asking for redundant
		 * updates like for different culling passes. */
		if ((curtime - m_previousAnimTime) < animTimeStep && curtime != m_previousAnimTime) {
			return;
		}

		// Sanity/debug print to make sure we're actually going at the fps we want (should be close to animTimeStep)
		// CM_Debug("Anim fps: " << 1.0 / (curtime - m_previousAnimTime));
		m_previousAnimTime = curtime;
	}

	m_animationPoolData.curtime = curtime;

	for (KX_GameObject *gameobj : m_animatedlist) {
		if (!gameobj->IsActionsSuspended()) {
			BLI_task_pool_push(m_animationPool, update_anim_thread_func, gameobj, false, TASK_PRIORITY_LOW);
		}
	}

	BLI_task_pool_work_and_wait(m_animationPool);
}

void KX_Scene::LogicUpdateFrame(double curtime)
{
	m_componentManager.UpdateComponents();
}

void KX_Scene::LogicEndFrame()
{
	/* Don't remove the objects from the euthanasy list here as the child objects of a deleted
	 * parent object are destructed directly from the sgnode in the same time the parent
	 * object is destructed. These child objects must be removed automatically from the
	 * euthanasy list to avoid double deletion in case the user ask to delete the child object
	 * explicitly. NewRemoveObject is the place to do it.
	 */
	while (!m_euthanasyobjects.empty()) {
		RemoveObject(m_euthanasyobjects.front());
	}

	//prepare obstacle simulation for new frame
	if (m_obstacleSimulation) {
		m_obstacleSimulation->UpdateObstacles();
	}

	for (KX_FontObject *font : m_fontlist) {
		font->UpdateTextFromProperty();
	}
}

void KX_Scene::UpdateParents()
{
	// We use the SG dynamic list
	SG_Node *node;

	while ((node = SG_Node::GetNextScheduled(m_sghead))) {
		node->UpdateWorldData();
	}

	// The list must be empty here
	BLI_assert(m_sghead.Empty());
	// Some nodes may be ready for reschedule, move them to schedule list for next time.
	while ((node = SG_Node::GetNextRescheduled(m_sghead))) {
		node->Schedule(m_sghead);
	}
}

RAS_MaterialBucket *KX_Scene::FindBucket(RAS_IPolyMaterial *polymat, bool &bucketCreated)
{
	return m_bucketmanager->FindBucket(polymat, bucketCreated);
}

void KX_Scene::RenderBuckets(const std::vector<KX_GameObject *>& objects, RAS_Rasterizer::DrawType drawingMode, const mt::mat3x4& cameratransform,
                             RAS_Rasterizer *rasty, RAS_OffScreen *offScreen)
{
	for (KX_GameObject *gameobj : objects) {
		/* This function update all mesh slot info (e.g culling, color, matrix) from the game object.
		 * It's done just before the render to be sure of the object color and visibility. */
		gameobj->UpdateBuckets();
	}

	m_bucketmanager->Renderbuckets(drawingMode, cameratransform, rasty, offScreen);
	KX_BlenderMaterial::EndFrame(rasty);
}

void KX_Scene::RenderTextureRenderers(KX_TextureRendererManager::RendererCategory category, RAS_Rasterizer *rasty,
                                      RAS_OffScreen *offScreen, KX_Camera *camera, const RAS_Rect& viewport, const RAS_Rect& area)
{
	m_rendererManager->Render(category, rasty, offScreen, camera, viewport, area);
}

void KX_Scene::UpdateObjectLods(KX_Camera *cam, const std::vector<KX_GameObject *>& objects)
{
	const mt::vec3& cam_pos = cam->NodeGetWorldPosition();
	const float lodfactor = cam->GetLodDistanceFactor();

	for (KX_GameObject *gameobj : objects) {
		gameobj->UpdateLod(this, cam_pos, lodfactor);
	}
}

void KX_Scene::SetLodHysteresis(bool active)
{
	m_isActivedHysteresis = active;
}

bool KX_Scene::IsActivedLodHysteresis() const
{
	return m_isActivedHysteresis;
}

void KX_Scene::SetLodHysteresisValue(int hysteresisvalue)
{
	m_lodHysteresisValue = hysteresisvalue;
}

int KX_Scene::GetLodHysteresisValue() const
{
	return m_lodHysteresisValue;
}

void KX_Scene::UpdateObjectActivity()
{
	if (!m_activityCulling) {
		return;
	}

	std::vector<mt::vec3, mt::simd_allocator<mt::vec3> > camPositions;

	for (KX_Camera *cam : m_cameralist) {
		if (cam->GetActivityCulling()) {
			camPositions.push_back(cam->NodeGetWorldPosition());
		}
	}

	// None cameras are using object activity culling?
	if (camPositions.size() == 0) {
		return;
	}

	for (KX_GameObject *gameobj : m_objectlist) {
		// If the object doesn't manage activity culling we don't compute distance.
		if (gameobj->GetActivityCullingInfo().m_flags == KX_GameObject::ActivityCullingInfo::ACTIVITY_NONE) {
			continue;
		}

		// For each camera compute the distance to objects and keep the minimum distance.
		const mt::vec3& obpos = gameobj->NodeGetWorldPosition();
		float dist = FLT_MAX;
		for (const mt::vec3& campos : camPositions) {
			// Keep the minimum distance.
			dist = std::min((obpos - campos).LengthSquared(), dist);
		}
		gameobj->UpdateActivity(dist);
	}
}

KX_NetworkMessageScene *KX_Scene::GetNetworkMessageScene() const
{
	return m_networkScene;
}

void KX_Scene::SetNetworkMessageScene(KX_NetworkMessageScene *netScene)
{
	m_networkScene = netScene;
}

PHY_IPhysicsEnvironment *KX_Scene::GetPhysicsEnvironment() const
{
	return m_physicsEnvironment;
}

void KX_Scene::SetPhysicsEnvironment(PHY_IPhysicsEnvironment *physEnv)
{
	m_physicsEnvironment = physEnv;
	if (m_physicsEnvironment) {
// 		KX_CollisionEventManager *collisionmgr = new KX_CollisionEventManager(m_logicmgr, physEnv);
		// TODO
	}
}

void KX_Scene::SetGravity(const mt::vec3& gravity)
{
	m_physicsEnvironment->SetGravity(gravity[0], gravity[1], gravity[2]);
}

mt::vec3 KX_Scene::GetGravity() const
{
	return m_physicsEnvironment->GetGravity();
}

void KX_Scene::SetSuspendedDelta(double suspendeddelta)
{
	m_suspendedDelta = suspendeddelta;
}

double KX_Scene::GetSuspendedDelta() const
{
	return m_suspendedDelta;
}

Scene *KX_Scene::GetBlenderScene() const
{
	return m_blenderScene;
}

static void MergeScene_GameObject(KX_GameObject *gameobj, KX_Scene *to, KX_Scene *from)
{
	// Graphics controller.
	PHY_IGraphicController *graphicCtrl = gameobj->GetGraphicController();
	if (graphicCtrl) {
		// Should update the m_cullingTree.
		graphicCtrl->SetPhysicsEnvironment(to->GetPhysicsEnvironment());
	}

	PHY_IPhysicsController *physicsCtrl = gameobj->GetPhysicsController();
	if (physicsCtrl) {
		physicsCtrl->SetPhysicsEnvironment(to->GetPhysicsEnvironment());
	}

	// SG_Node can hold a scene reference.
	SG_Node *sg = gameobj->GetNode();
	if (sg) {
		if (sg->GetClientInfo() == from) {
			sg->SetClientInfo(to);

			// Make sure to grab the children too since they might not be tied to a game object.
			const NodeList& children = sg->GetChildren();
			for (SG_Node *child : children) {
				child->SetClientInfo(to);
			}
		}
	}
	// If the object is a light, update it's scene.
	if (gameobj->GetObjectType() == KX_GameObject::OBJECT_TYPE_LIGHT) {
		static_cast<KX_LightObject *>(gameobj)->UpdateScene(to);
	}

	// All armatures should be in the animated object list to be umpdated.
	if (gameobj->GetObjectType() == KX_GameObject::OBJECT_TYPE_ARMATURE) {
		to->AddAnimatedObject(gameobj);
	}
}

bool KX_Scene::Merge(KX_Scene *other)
{
	PHY_IPhysicsEnvironment *env = this->GetPhysicsEnvironment();
	PHY_IPhysicsEnvironment *env_other = other->GetPhysicsEnvironment();

	if ((env == nullptr) != (env_other == nullptr)) {
		CM_FunctionError("physics scenes type differ, aborting\n\tsource " << (int)(env != nullptr) << ", target " << (int)(env_other != nullptr));
		return false;
	}

	m_bucketmanager->MergeBucketManager(other->GetBucketManager());
	m_boundingBoxManager->Merge(other->GetBoundingBoxManager());
	m_rendererManager->Merge(other->GetTextureRendererManager());

	for (KX_GameObject *gameobj : other->GetObjectList()) {
		MergeScene_GameObject(gameobj, this, other);

		// Add properties to debug list for LibLoad objects.
		if (KX_GetActiveEngine()->GetFlag(KX_KetsjiEngine::AUTO_ADD_DEBUG_PROPERTIES)) {
			AddObjectDebugProperties(gameobj);
		}
	}

	for (KX_GameObject *gameobj : other->GetInactiveList()) {
		MergeScene_GameObject(gameobj, this, other);
	}

	if (env) {
		env->MergeEnvironment(env_other);
		EXP_ListValue<KX_GameObject>& otherObjects = other->GetObjectList();

		// List of all physics objects to merge (needed by ReplicateConstraints).
		std::vector<KX_GameObject *> physicsObjects;
		for (KX_GameObject *gameobj : otherObjects) {
			if (gameobj->GetPhysicsController()) {
				physicsObjects.push_back(gameobj);
			}
		}

		for (KX_GameObject *gameobj : physicsObjects) {
			// Replicate all constraints in the right physics environment.
			gameobj->ReplicateConstraints(m_physicsEnvironment, physicsObjects);
		}
	}

	m_objectlist.MergeList(other->GetObjectList());
	m_inactivelist.MergeList(other->GetInactiveList());
	m_parentlist.MergeList(other->GetRootParentList());
	m_lightlist.MergeList(other->GetLightList());
	m_cameralist.MergeList(other->GetCameraList());
	m_fontlist.MergeList(other->GetFontList());

	// Grab any timer properties from the other scene.
	/*SCA_TimeEventManager *timemgr_other = other->GetTimeEventManager();
	std::vector<EXP_Value *> times = timemgr_other->GetTimeValues();

	for (EXP_Value *time : times) {
		m_timemgr->AddTimeProperty(time);
	} TODO */

	m_resssources.Merge(other->GetResources());

	return true;
}

void KX_Scene::RemoveTagged()
{
	// removed tagged objects and meshes
	std::array<EXP_ListValue<KX_GameObject> *, 2> obj_lists{{&m_objectlist, &m_inactivelist}};

	for (EXP_ListValue<KX_GameObject> *obs : obj_lists) {
		for (int ob_idx = 0; ob_idx < obs->GetCount(); ob_idx++) {
			KX_GameObject *gameobj = obs->GetValue(ob_idx);
			if (IS_TAGGED(gameobj->GetBlenderObject())) {
				int size_before = obs->GetCount();

				RemoveObject(gameobj);

				if (size_before != obs->GetCount()) {
					ob_idx--;
				}
				else {
					CM_Error("could not remove \"" << gameobj->GetName() << "\"");
				}
			}
			else {
				gameobj->RemoveTaggedActions();

				// Free the mesh, we could be referecing a linked one.
				for (KX_Mesh *meshobj : gameobj->GetMeshList()) {
					if (IS_TAGGED(meshobj->GetMesh())) {
						gameobj->RemoveMeshes(); /* XXX - slack, should only remove meshes that are library items but mostly objects only have 1 mesh */
						break;
					}
					else {
						// also free the mesh if it's using a tagged material
						for (RAS_MeshMaterial *meshmat : meshobj->GetMeshMaterialList()) {
							if (IS_TAGGED(meshmat->GetBucket()->GetPolyMaterial()->GetBlenderMaterial())) {
								gameobj->RemoveMeshes(); // XXX - slack, same as above
								break;
							}
						}
					}
				}
			}
		}
	}

	m_resssources.RemoveTagged(this);
}

KX_2DFilterManager *KX_Scene::Get2DFilterManager() const
{
	return m_filterManager;
}

RAS_OffScreen *KX_Scene::Render2DFilters(RAS_Rasterizer *rasty, RAS_ICanvas *canvas, RAS_OffScreen *inputofs, RAS_OffScreen *targetofs)
{
	return m_filterManager->RenderFilters(rasty, canvas, inputofs, targetofs);
}

KX_ObstacleSimulation *KX_Scene::GetObstacleSimulation()
{
	return m_obstacleSimulation;
}

void KX_Scene::SetObstacleSimulation(KX_ObstacleSimulation *obstacleSimulation)
{
	m_obstacleSimulation = obstacleSimulation;
}

#ifdef WITH_PYTHON

void KX_Scene::RunDrawingCallbacks(DrawingCallbackType callbackType, KX_Camera *camera)
{
	PyObject *list = m_drawCallbacks[callbackType];
	if (!list || PyList_GET_SIZE(list) == 0) {
		return;
	}

	if (camera) {
		PyObject *args[1] = {camera->GetProxy()};
		EXP_RunPythonCallBackList(list, args, 0, 1);
	}
	else {
		EXP_RunPythonCallBackList(list, nullptr, 0, 0);
	}
}

PyTypeObject KX_Scene::Type = {
	PyVarObject_HEAD_INIT(nullptr, 0)
	"KX_Scene",
	sizeof(EXP_PyObjectPlus_Proxy),
	0,
	py_base_dealloc,
	0,
	0,
	0,
	0,
	py_base_repr,
	0,
	&Sequence,
	&Mapping,
	0, 0, 0, 0, 0, 0,
	Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
	0, 0, 0, 0, 0, 0, 0,
	Methods,
	0,
	0,
	&EXP_Value::Type,
	0, 0, 0, 0, 0, 0,
	py_base_new
};

PyMethodDef KX_Scene::Methods[] = {
	EXP_PYMETHODTABLE(KX_Scene, addObject),
	EXP_PYMETHODTABLE(KX_Scene, end),
	EXP_PYMETHODTABLE(KX_Scene, restart),
	EXP_PYMETHODTABLE(KX_Scene, replace),
	EXP_PYMETHODTABLE(KX_Scene, suspend),
	EXP_PYMETHODTABLE(KX_Scene, resume),
	EXP_PYMETHODTABLE(KX_Scene, drawObstacleSimulation),

	// Sict style access.
	EXP_PYMETHODTABLE(KX_Scene, get),

	{nullptr, nullptr} // Sentinel
};
static PyObject *Map_GetItem(PyObject *self_v, PyObject *item)
{
	KX_Scene *self = static_cast<KX_Scene *>EXP_PROXY_REF(self_v);
	const char *attr_str = _PyUnicode_AsString(item);
	PyObject *pyconvert;

	if (!self) {
		PyErr_SetString(PyExc_SystemError, "val = scene[key]: KX_Scene, " EXP_PROXY_ERROR_MSG);
		return nullptr;
	}

	if (!self->m_attrDict) {
		self->m_attrDict = PyDict_New();
	}

	if (self->m_attrDict && (pyconvert = PyDict_GetItem(self->m_attrDict, item))) {

		if (attr_str) {
			PyErr_Clear();
		}
		Py_INCREF(pyconvert);
		return pyconvert;
	}
	else {
		if (attr_str) {
			PyErr_Format(PyExc_KeyError, "value = scene[key]: KX_Scene, key \"%s\" does not exist", attr_str);
		}
		else {
			PyErr_SetString(PyExc_KeyError, "value = scene[key]: KX_Scene, key does not exist");
		}
		return nullptr;
	}

}

static int Map_SetItem(PyObject *self_v, PyObject *key, PyObject *val)
{
	KX_Scene *self = static_cast<KX_Scene *>EXP_PROXY_REF(self_v);
	const char *attr_str = _PyUnicode_AsString(key);
	if (!attr_str) {
		PyErr_Clear();
	}

	if (!self) {
		PyErr_SetString(PyExc_SystemError, "scene[key] = value: KX_Scene, " EXP_PROXY_ERROR_MSG);
		return -1;
	}

	if (!self->m_attrDict) {
		self->m_attrDict = PyDict_New();
	}

	if (!val) {
		// del ob["key"]
		int del = 0;

		if (self->m_attrDict) {
			del |= (PyDict_DelItem(self->m_attrDict, key) == 0) ? 1 : 0;
		}

		if (del == 0) {
			if (attr_str) {
				PyErr_Format(PyExc_KeyError, "scene[key] = value: KX_Scene, key \"%s\" could not be set", attr_str);
			}
			else {
				PyErr_SetString(PyExc_KeyError, "del scene[key]: KX_Scene, key could not be deleted");
			}
			return -1;
		}
		else if (self->m_attrDict) {
			// PyDict_DelItem sets an error when it fails.
			PyErr_Clear();
		}
	}
	else {
		// ob["key"] = value
		int set = 0;

		// Lazy init.
		if (!self->m_attrDict) {
			self->m_attrDict = PyDict_New();
		}

		if (PyDict_SetItem(self->m_attrDict, key, val) == 0) {
			set = 1;
		}
		else {
			PyErr_SetString(PyExc_KeyError, "scene[key] = value: KX_Scene, key not be added to internal dictionary");
		}

		if (set == 0) {
			// Pythons error value.
			return -1;

		}
	}

	// Success.
	return 0;
}

static int Seq_Contains(PyObject *self_v, PyObject *value)
{
	KX_Scene *self = static_cast<KX_Scene *>EXP_PROXY_REF(self_v);

	if (!self) {
		PyErr_SetString(PyExc_SystemError, "val in scene: KX_Scene, " EXP_PROXY_ERROR_MSG);
		return -1;
	}

	if (!self->m_attrDict) {
		self->m_attrDict = PyDict_New();
	}

	if (self->m_attrDict && PyDict_GetItem(self->m_attrDict, value)) {
		return 1;
	}

	return 0;
}

PyMappingMethods KX_Scene::Mapping = {
	(lenfunc)nullptr, // inquiry mp_length
	(binaryfunc)Map_GetItem, // binaryfunc mp_subscript
	(objobjargproc)Map_SetItem, // objobjargproc mp_ass_subscript
};

PySequenceMethods KX_Scene::Sequence = {
	nullptr, // Cant set the len otherwise it can evaluate as false.
	nullptr, // sq_concat
	nullptr, // sq_repeat
	nullptr, // sq_item
	nullptr, // sq_slice
	nullptr, // sq_ass_item
	nullptr, // sq_ass_slice
	(objobjproc)Seq_Contains, // sq_contains
	(binaryfunc)nullptr, // sq_inplace_concat
	(ssizeargfunc)nullptr, // sq_inplace_repeat
};

KX_Camera *KX_Scene::pyattr_get_active_camera()
{
	return m_activeCamera;
}

bool KX_Scene::pyattr_set_active_camera(PyObject *value)
{
	KX_Camera *camOb;

	if (!ConvertPythonToCamera(this, value, &camOb, false, "scene.active_camera = value: KX_Scene")) {
		return false;
	}

	m_activeCamera = camOb;
	return true;
}

KX_Camera *KX_Scene::pyattr_get_overrideCullingCamera()
{
	return m_overrideCullingCamera;
}

bool KX_Scene::pyattr_set_overrideCullingCamera(PyObject *value)
{
	KX_Camera *cam;

	if (!ConvertPythonToCamera(this, value, &cam, true, "scene.active_camera = value: KX_Scene")) {
		return false;
	}

	m_overrideCullingCamera = cam;
	return true;
}

static std::map<const std::string, KX_Scene::DrawingCallbackType> callbacksTable = {
	{"pre_draw", KX_Scene::PRE_DRAW},
	{"pre_draw_setup", KX_Scene::PRE_DRAW_SETUP},
	{"post_draw", KX_Scene::POST_DRAW}
};

PyObject *KX_Scene::pyattr_get_drawing_callback(const EXP_Attribute *attrdef)
{
	const DrawingCallbackType type = callbacksTable[attrdef->m_name];
	if (!m_drawCallbacks[type]) {
		m_drawCallbacks[type] = PyList_New(0);
	}

	Py_INCREF(m_drawCallbacks[type]);

	return m_drawCallbacks[type];
}

bool KX_Scene::pyattr_set_drawing_callback(PyObject *value, const EXP_Attribute *attrdef)
{
	if (!PyList_CheckExact(value)) {
		attrdef->PrintError(" = list: Expected a list.");
		return false;
	}

	const DrawingCallbackType type = callbacksTable[attrdef->m_name];

	Py_XDECREF(m_drawCallbacks[type]);

	Py_INCREF(value);
	m_drawCallbacks[type] = value;

	return true;
}

mt::vec3 KX_Scene::pyattr_get_gravity()
{
	return GetGravity();
}

void KX_Scene::pyattr_set_gravity(const mt::vec3& value)
{
	SetGravity(value);
}

EXP_Attribute KX_Scene::Attributes[] = {
	EXP_ATTRIBUTE_RO("name", m_name),
	EXP_ATTRIBUTE_RO("objects", m_objectlist),
	EXP_ATTRIBUTE_RO("objectsInactive", m_inactivelist),
	EXP_ATTRIBUTE_RO("lights", m_lightlist),
	EXP_ATTRIBUTE_RO("texts", m_fontlist),
	EXP_ATTRIBUTE_RO("cameras", m_cameralist),
	EXP_ATTRIBUTE_RO("filterManager", m_filterManager),
	EXP_ATTRIBUTE_RO("world", m_worldinfo),
	EXP_ATTRIBUTE_RW_FUNCTION("active_camera", pyattr_get_active_camera, pyattr_set_active_camera),
	EXP_ATTRIBUTE_RW_FUNCTION("overrideCullingCamera", pyattr_get_overrideCullingCamera, pyattr_set_overrideCullingCamera),
	EXP_ATTRIBUTE_RW_FUNCTION("pre_draw", pyattr_get_drawing_callback, pyattr_set_drawing_callback),
	EXP_ATTRIBUTE_RW_FUNCTION("post_draw", pyattr_get_drawing_callback, pyattr_set_drawing_callback),
	EXP_ATTRIBUTE_RW_FUNCTION("pre_draw_setup", pyattr_get_drawing_callback, pyattr_set_drawing_callback),
	EXP_ATTRIBUTE_RW_FUNCTION("gravity", pyattr_get_gravity, pyattr_set_gravity),
	EXP_ATTRIBUTE_RO("suspended", m_suspend),
	EXP_ATTRIBUTE_RO("activityCulling", m_activityCulling),
	EXP_ATTRIBUTE_RO("dbvt_culling", m_dbvtCulling),
	EXP_ATTRIBUTE_NULL // Sentinel
};

EXP_PYMETHODDEF_DOC(KX_Scene, addObject,
                    "addObject(object, other, time=0)\n"
                    "Returns the added object.\n")
{
	PyObject *pyob, *pyreference = Py_None;
	KX_GameObject *ob, *reference;

	float time = 0.0f;

	if (!PyArg_ParseTuple(args, "O|Of:addObject", &pyob, &pyreference, &time)) {
		return nullptr;
	}

	if (!ConvertPythonToGameObject(this, pyob, &ob, false, "scene.addObject(object, reference, time): KX_Scene (first argument)") ||
	    !ConvertPythonToGameObject(this, pyreference, &reference, true, "scene.addObject(object, reference, time): KX_Scene (second argument)")) {
		return nullptr;
	}

	if (!m_inactivelist.SearchValue(ob)) {
		PyErr_Format(PyExc_ValueError, "scene.addObject(object, reference, time): KX_Scene (first argument): object must be in an inactive layer");
		return nullptr;
	}
	KX_GameObject *replica = AddReplicaObject(ob, reference, time);

	return replica->GetProxy();
}

EXP_PYMETHODDEF_DOC(KX_Scene, end,
                    "end()\n"
                    "Removes this scene from the game.\n")
{

	KX_GetActiveEngine()->RemoveScene(m_name);

	Py_RETURN_NONE;
}

EXP_PYMETHODDEF_DOC(KX_Scene, restart,
                    "restart()\n"
                    "Restarts this scene.\n")
{
	KX_GetActiveEngine()->ReplaceScene(m_name, m_name);

	Py_RETURN_NONE;
}

EXP_PYMETHODDEF_DOC(KX_Scene, replace,
                    "replace(newScene)\n"
                    "Replaces this scene with another one.\n"
                    "Return True if the new scene exists and scheduled for replacement, False otherwise.\n")
{
	char *name;

	if (!PyArg_ParseTuple(args, "s:replace", &name)) {
		return nullptr;
	}

	if (KX_GetActiveEngine()->ReplaceScene(m_name, name)) {
		Py_RETURN_TRUE;
	}

	Py_RETURN_FALSE;
}

EXP_PYMETHODDEF_DOC(KX_Scene, suspend,
                    "suspend()\n"
                    "Suspends this scene.\n")
{
	Suspend();

	Py_RETURN_NONE;
}

EXP_PYMETHODDEF_DOC(KX_Scene, resume,
                    "resume()\n"
                    "Resumes this scene.\n")
{
	Resume();

	Py_RETURN_NONE;
}

EXP_PYMETHODDEF_DOC(KX_Scene, drawObstacleSimulation,
                    "drawObstacleSimulation()\n"
                    "Draw debug visualization of obstacle simulation.\n")
{
	if (GetObstacleSimulation()) {
		GetObstacleSimulation()->DrawObstacles();
	}

	Py_RETURN_NONE;
}

EXP_PYMETHODDEF_DOC(KX_Scene, get, "")
{
	PyObject *key;
	PyObject *def = Py_None;
	PyObject *ret;

	if (!PyArg_ParseTuple(args, "O|O:get", &key, &def)) {
		return nullptr;
	}

	if (m_attrDict && (ret = PyDict_GetItem(m_attrDict, key))) {
		Py_INCREF(ret);
		return ret;
	}

	Py_INCREF(def);
	return def;
}

bool ConvertPythonToScene(PyObject *value, KX_Scene **scene, bool py_none_ok, const char *error_prefix)
{
	if (value == nullptr) {
		PyErr_Format(PyExc_TypeError, "%s, python pointer nullptr, should never happen", error_prefix);
		*scene = nullptr;
		return false;
	}

	if (value == Py_None) {
		*scene = nullptr;

		if (py_none_ok) {
			return true;
		}
		else {
			PyErr_Format(PyExc_TypeError, "%s, expected KX_Scene or a KX_Scene name, None is invalid", error_prefix);
			return false;
		}
	}

	if (PyUnicode_Check(value)) {
		*scene = KX_GetActiveEngine()->FindScene(std::string(_PyUnicode_AsString(value)));

		if (*scene) {
			return true;
		}
		else {
			PyErr_Format(PyExc_ValueError, "%s, requested name \"%s\" did not match any in game", error_prefix, _PyUnicode_AsString(value));
			return false;
		}
	}

	if (PyObject_TypeCheck(value, &KX_Scene::Type)) {
		*scene = static_cast<KX_Scene *>EXP_PROXY_REF(value);

		// Sets the error.
		if (*scene == nullptr) {
			PyErr_Format(PyExc_SystemError, "%s, " EXP_PROXY_ERROR_MSG, error_prefix);
			return false;
		}

		return true;
	}

	*scene = nullptr;

	if (py_none_ok) {
		PyErr_Format(PyExc_TypeError, "%s, expect a KX_Scene, a string or None", error_prefix);
	}
	else {
		PyErr_Format(PyExc_TypeError, "%s, expect a KX_Scene or a string", error_prefix);
	}

	return false;
}

#endif  // WITH_PYTHON
