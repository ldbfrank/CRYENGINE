// Copyright 2001-2016 Crytek GmbH / Crytek Group. All rights reserved.

#include "StdAfx.h"
#include "FileLoader.h"

#include "FileWriter.h"
#include "SystemAssetsManager.h"
#include "SystemControlsModel.h"
#include "AudioControlsEditorPlugin.h"

#include <CryAudio/IAudioSystem.h>
#include <CryString/StringUtils.h>
#include <CrySystem/File/CryFile.h>
#include <CrySystem/ISystem.h>
#include <CryString/CryPath.h>
#include <QtUtil.h>
#include <IEditor.h>
#include <ConfigurationManager.h>

#include <QRegularExpression>

namespace ACE
{
//////////////////////////////////////////////////////////////////////////
ESystemItemType TagToType(string const& tag)
{
	ESystemItemType type;

	if (tag == CryAudio::s_szSwitchTag)
	{
		type = ESystemItemType::Switch;
	}
	else if (tag == CryAudio::s_szStateTag)
	{
		type = ESystemItemType::State;
	}
	else if (tag == CryAudio::s_szEnvironmentTag)
	{
		type = ESystemItemType::Environment;
	}
	else if (tag == CryAudio::s_szParameterTag)
	{
		type = ESystemItemType::Parameter;
	}
	else if (tag == CryAudio::s_szTriggerTag)
	{
		type = ESystemItemType::Trigger;
	}
	else if (tag == CryAudio::s_szPreloadRequestTag)
	{
		type = ESystemItemType::Preload;
	}
	else
	{
		type = ESystemItemType::Invalid;
	}

	return type;
}

//////////////////////////////////////////////////////////////////////////
CFileLoader::CFileLoader(CSystemAssetsManager& assetsManager)
	: m_assetsManager(assetsManager)
	, m_errorCodeMask(EErrorCode::NoError)
{}

//////////////////////////////////////////////////////////////////////////
void CFileLoader::LoadAll()
{
	LoadScopes();
	LoadControls();
}

//////////////////////////////////////////////////////////////////////////
void CFileLoader::LoadControls()
{
	// load the global controls
	LoadAllLibrariesInFolder(m_assetsManager.GetConfigFolderPath(), "");

	// load the level specific controls
	_finddata_t fd;
	ICryPak* pCryPak = gEnv->pCryPak;
	intptr_t handle = pCryPak->FindFirst(m_assetsManager.GetConfigFolderPath() + CryAudio::s_szLevelsFolderName + CRY_NATIVE_PATH_SEPSTR + "*.*", &fd);

	if (handle != -1)
	{
		do
		{
			if (fd.attrib & _A_SUBDIR)
			{
				string name = fd.name;

				if (name != "." && name != "..")
				{
					LoadAllLibrariesInFolder(m_assetsManager.GetConfigFolderPath(), name);

					if (!m_assetsManager.ScopeExists(fd.name))
					{
						// if the control doesn't exist it
						// means it is not a real level in the
						// project so it is flagged as LocalOnly
						m_assetsManager.AddScope(fd.name, true);
					}
				}
			}
		}
		while (pCryPak->FindNext(handle, &fd) >= 0);

		pCryPak->FindClose(handle);
	}

	CreateDefaultControls();
}

//////////////////////////////////////////////////////////////////////////
void CFileLoader::LoadAllLibrariesInFolder(string const& folderPath, string const& level)
{
	string path = folderPath;

	if (!level.empty())
	{
		path = path + CryAudio::s_szLevelsFolderName + CRY_NATIVE_PATH_SEPSTR + level + CRY_NATIVE_PATH_SEPSTR;
	}

	string const searchPath = path + "*.xml";
	ICryPak* const pCryPak = gEnv->pCryPak;
	_finddata_t fd;
	intptr_t const handle = pCryPak->FindFirst(searchPath, &fd);

	if (handle != -1)
	{
		do
		{
			string filename = path + fd.name;
			XmlNodeRef const root = GetISystem()->LoadXmlFromFile(filename);

			if (root != nullptr)
			{
				string const tag = root->getTag();

				if (tag == CryAudio::s_szRootNodeTag)
				{
					m_loadedFilenames.insert(filename.MakeLower());
					string file = fd.name;

					if (root->haveAttr(CryAudio::s_szNameAttribute))
					{
						file = root->getAttr(CryAudio::s_szNameAttribute);
					}

					int version = 1;
					root->getAttr(CryAudio::s_szVersionAttribute, version);
					PathUtil::RemoveExtension(file);
					LoadControlsLibrary(root, folderPath, level, file, version);
				}
			}
			else
			{
				CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_ERROR, "[Audio Controls Editor] Failed parsing game sound file %s", filename);
			}
		}
		while (pCryPak->FindNext(handle, &fd) >= 0);

		pCryPak->FindClose(handle);
	}
}

//////////////////////////////////////////////////////////////////////////
CSystemAsset* CFileLoader::AddUniqueFolderPath(CSystemAsset* pParent, QString const& path)
{
	QStringList const folderNames = path.split(QRegularExpression(R"((\\|\/))"), QString::SkipEmptyParts);

	int const size = folderNames.length();

	for (int i = 0; i < size; ++i)
	{
		if (!folderNames[i].isEmpty())
		{
			CSystemAsset* const pChild = m_assetsManager.CreateFolder(QtUtil::ToString(folderNames[i]), pParent);

			if (pChild != nullptr)
			{
				pParent = pChild;
			}
		}
	}

	return pParent;
}

//////////////////////////////////////////////////////////////////////////
void CFileLoader::LoadControlsLibrary(XmlNodeRef const pRoot, string const& filepath, string const& level, string const& filename, uint32 const version)
{
  // Always create a library file, even if no proper formatting is present.
	CSystemLibrary* const pLibrary = m_assetsManager.CreateLibrary(filename);

	if (pLibrary != nullptr)
	{
		int const controlTypeCount = pRoot->getChildCount();

		for (int i = 0; i < controlTypeCount; ++i)
		{
			XmlNodeRef const pNode = pRoot->getChild(i);

			if (pNode != nullptr)
			{
				if (pNode->isTag(CryAudio::s_szEditorDataTag))
				{
					LoadEditorData(pNode, *pLibrary);
				}
				else
				{
					Scope const scope = level.empty() ? Utils::GetGlobalScope() : m_assetsManager.GetScope(level);
					int const numControls = pNode->getChildCount();

					for (int j = 0; j < numControls; ++j)
					{
						LoadControl(pNode->getChild(j), scope, version, pLibrary);
					}
				}
			}
		}
	}
}

//////////////////////////////////////////////////////////////////////////
CSystemControl* CFileLoader::LoadControl(XmlNodeRef const pNode, Scope const scope, uint32 const version, CSystemAsset* const pParentItem)
{
	CSystemControl* pControl = nullptr;

	if (pNode != nullptr)
	{
		CSystemAsset* const pFolderItem = AddUniqueFolderPath(pParentItem, QtUtil::ToQString(pNode->getAttr(s_szPathAttribute)));

		if (pFolderItem != nullptr)
		{
			string const name = pNode->getAttr(CryAudio::s_szNameAttribute);
			ESystemItemType const controlType = TagToType(pNode->getTag());

			pControl = m_assetsManager.CreateControl(name, controlType, pFolderItem);

			if (pControl != nullptr)
			{
				switch (controlType)
				{
				case ESystemItemType::Trigger:
					{
						float radius = 0.0f;
						pNode->getAttr(CryAudio::s_szRadiusAttribute, radius);
						pControl->SetRadius(radius);
						LoadConnections(pNode, pControl);
					}
					break;
				case ESystemItemType::Switch:
					{
						int const stateCount = pNode->getChildCount();

						for (int i = 0; i < stateCount; ++i)
						{
							LoadControl(pNode->getChild(i), scope, version, pControl);
						}
					}
				break;
				case ESystemItemType::Preload:
					LoadPreloadConnections(pNode, pControl, version);
					break;
				default:
					LoadConnections(pNode, pControl);
					break;
				}

				pControl->SetScope(scope);
			}
		}
	}

	return pControl;
}

//////////////////////////////////////////////////////////////////////////
void CFileLoader::LoadScopes()
{
	string levelsFolder = CryAudio::s_szLevelsFolderName;
	levelsFolder += CRY_NATIVE_PATH_SEPSTR;
	LoadScopesImpl(levelsFolder);
}

//////////////////////////////////////////////////////////////////////////
void CFileLoader::LoadScopesImpl(string const& sLevelsFolder)
{
	// TODO: consider moving the file enumeration to a background thread to speed up the editor startup time.

	_finddata_t fd;
	ICryPak* const pCryPak = gEnv->pCryPak;
	intptr_t const handle = pCryPak->FindFirst(sLevelsFolder + CRY_NATIVE_PATH_SEPSTR "*.*", &fd);

	if (handle != -1)
	{
		do
		{
			string name = fd.name;

			if ((name != ".") && (name != "..") && !name.empty())
			{
				if (fd.attrib & _A_SUBDIR)
				{
					LoadScopesImpl(sLevelsFolder + CRY_NATIVE_PATH_SEPSTR + name);
				}
				else
				{
					if (strcmp(PathUtil::GetExt(name), "level") == 0)
					{
						PathUtil::RemoveExtension(name);
						m_assetsManager.AddScope(name);
					}
				}
			}
		}
		while (pCryPak->FindNext(handle, &fd) >= 0);

		pCryPak->FindClose(handle);
	}
}

//////////////////////////////////////////////////////////////////////////
std::set<string> CFileLoader::GetLoadedFilenamesList()
{
	return m_loadedFilenames;
}

//////////////////////////////////////////////////////////////////////////
void CFileLoader::CreateDefaultControls()
{
	// Create default controls if they don't exist.
	// These controls need to always exist in your project!
	bool wasModified = false;
	CSystemAsset* const pLibrary = static_cast<CSystemAsset*>(m_assetsManager.CreateLibrary(CryAudio::s_szDefaultLibraryName));

	if (pLibrary != nullptr)
	{
		CSystemControl* pControl = m_assetsManager.FindControl(CryAudio::s_szGetFocusTriggerName, ESystemItemType::Trigger);

		if (pControl == nullptr)
		{
			pControl = m_assetsManager.CreateControl(CryAudio::s_szGetFocusTriggerName, ESystemItemType::Trigger, pLibrary);
			wasModified = true;
		}

		if (pControl != nullptr)
		{
			pControl->SetDefaultControl(true);
		}

		pControl = m_assetsManager.FindControl(CryAudio::s_szLoseFocusTriggerName, ESystemItemType::Trigger);

		if (pControl == nullptr)
		{
			pControl = m_assetsManager.CreateControl(CryAudio::s_szLoseFocusTriggerName, ESystemItemType::Trigger, pLibrary);
			wasModified = true;
		}

		if (pControl != nullptr)
		{
			pControl->SetDefaultControl(true);
		}

		pControl = m_assetsManager.FindControl(CryAudio::s_szMuteAllTriggerName, ESystemItemType::Trigger);

		if (pControl == nullptr)
		{
			pControl = m_assetsManager.CreateControl(CryAudio::s_szMuteAllTriggerName, ESystemItemType::Trigger, pLibrary);
			wasModified = true;
		}

		if (pControl != nullptr)
		{
			pControl->SetDefaultControl(true);
		}

		pControl = m_assetsManager.FindControl(CryAudio::s_szUnmuteAllTriggerName, ESystemItemType::Trigger);

		if (pControl == nullptr)
		{
			pControl = m_assetsManager.CreateControl(CryAudio::s_szUnmuteAllTriggerName, ESystemItemType::Trigger, pLibrary);
			wasModified = true;
		}

		if (pControl != nullptr)
		{
			pControl->SetDefaultControl(true);
		}

		pControl = m_assetsManager.FindControl(CryAudio::s_szDoNothingTriggerName, ESystemItemType::Trigger);

		if (pControl == nullptr)
		{
			pControl = m_assetsManager.CreateControl(CryAudio::s_szDoNothingTriggerName, ESystemItemType::Trigger, pLibrary);
			wasModified = true;
		}

		if (pControl != nullptr)
		{
			pControl->SetDefaultControl(true);
		}

		pControl = m_assetsManager.FindControl(CryAudio::s_szAbsoluteVelocityParameterName, ESystemItemType::Parameter);

		if (pControl == nullptr)
		{
			pControl = m_assetsManager.CreateControl(CryAudio::s_szAbsoluteVelocityParameterName, ESystemItemType::Parameter, pLibrary);
			wasModified = true;
		}

		if (pControl != nullptr)
		{
			pControl->SetDefaultControl(true);
		}

		pControl = m_assetsManager.FindControl(CryAudio::s_szRelativeVelocityParameterName, ESystemItemType::Parameter);

		if (pControl == nullptr)
		{
			pControl = m_assetsManager.CreateControl(CryAudio::s_szRelativeVelocityParameterName, ESystemItemType::Parameter, pLibrary);
			wasModified = true;
		}

		if (pControl != nullptr)
		{
			pControl->SetDefaultControl(true);
		}

		{
			SwitchStates const states{ CryAudio::s_szIgnoreStateName, CryAudio::s_szAdaptiveStateName, CryAudio::s_szLowStateName, CryAudio::s_szMediumStateName, CryAudio::s_szHighStateName };
			
			if (CreateDefaultSwitch(pLibrary, CryAudio::s_szOcclCalcSwitchName, CryAudio::s_szOcclCalcSwitchRequestName, states))
			{
				wasModified = true;
			}
		}

		{
			SwitchStates const states{ CryAudio::s_szOnStateName, CryAudio::s_szOffStateName };

			if (CreateDefaultSwitch(pLibrary, CryAudio::s_szAbsoluteVelocityTrackingSwitchName, CryAudio::s_szAbsoluteVelocityTrackingSwitchName, states))
			{
				wasModified = true;
			}
			if (CreateDefaultSwitch(pLibrary, CryAudio::s_szRelativeVelocityTrackingSwitchName, CryAudio::s_szRelativeVelocityTrackingSwitchName, states))
			{
				wasModified = true;
			}
		}

		if (pLibrary->ChildCount() == 0)
		{
			m_assetsManager.DeleteItem(pLibrary);
		}
		else if (wasModified)
		{
			pLibrary->SetModified(true, true);
		}
	}
}

//////////////////////////////////////////////////////////////////////////
bool CFileLoader::CreateDefaultSwitch(CSystemAsset* const pLibrary, char const* const szExternalName, char const* const szInternalName, SwitchStates const& states)
{
	bool wasModified = false;
	CSystemControl* pSwitch = m_assetsManager.FindControl(szExternalName, ESystemItemType::Switch);

	if (pSwitch != nullptr)
	{
		// Remove any states that shouldn't be part of the default control
		size_t childIndex = 0;
		size_t childCount = pSwitch->ChildCount();

		while (childIndex < childCount)
		{
			CSystemAsset const* const pChild = pSwitch->GetChild(childIndex);

			if (pChild != nullptr)
			{
				bool shouldRemoveChild = true;

				for (auto const& szStateName : states)
				{
					if (strcmp(pChild->GetName().c_str(), szStateName) == 0)
					{
						++childIndex;
						shouldRemoveChild = false;
						break;
					}
				}

				if (shouldRemoveChild)
				{
					pSwitch->RemoveChild(pChild);
					childCount = pSwitch->ChildCount();
					wasModified = true;
				}
			}
		}
	}
	else
	{
		pSwitch = m_assetsManager.CreateControl(szExternalName, ESystemItemType::Switch, pLibrary);
		wasModified = true;
	}

	for (auto const& szStateName : states)
	{
		CSystemControl* pState = nullptr;

		size_t const stateCount = pSwitch->ChildCount();

		for (size_t i = 0; i < stateCount; ++i)
		{
			CSystemControl* pChild = static_cast<CSystemControl*>(pSwitch->GetChild(i));
			if ((pChild != nullptr) && (strcmp(pChild->GetName().c_str(), szStateName) == 0) && (pChild->GetType() == ESystemItemType::State))
			{
				pState = pChild;
				break;
			}
		}

		if (pState == nullptr)
		{
			pState = m_assetsManager.CreateControl(szStateName, ESystemItemType::State, pSwitch);

			XmlNodeRef const pRequestNode = GetISystem()->CreateXmlNode(CryAudio::s_szSwitchRequestTag);
			pRequestNode->setAttr(CryAudio::s_szNameAttribute, szInternalName);
			XmlNodeRef const pValueNode = pRequestNode->createNode(CryAudio::s_szValueTag);
			pValueNode->setAttr(CryAudio::s_szNameAttribute, szStateName);
			pRequestNode->addChild(pValueNode);

			pState->AddRawXMLConnection(pRequestNode, false);
		}
	}

	pSwitch->SetDefaultControl(true);
	pSwitch->SetHiddenDefault(true);
	return wasModified;
}

//////////////////////////////////////////////////////////////////////////
void CFileLoader::LoadConnections(XmlNodeRef const pRoot, CSystemControl* const pControl)
{
	// The radius might change because of the attenuation matching option
	// so we check here to inform the user if their data is outdated.
	float const radius = pControl->GetRadius();
	int const numChildren = pRoot->getChildCount();

	for (int i = 0; i < numChildren; ++i)
	{
		XmlNodeRef const pNode = pRoot->getChild(i);
		pControl->LoadConnectionFromXML(pNode);
	}

	if (radius != pControl->GetRadius())
	{
		m_errorCodeMask |= EErrorCode::NonMatchedActivityRadius;
		pControl->SetModified(true, true);
	}
}

//////////////////////////////////////////////////////////////////////////
void CFileLoader::LoadPreloadConnections(XmlNodeRef const pNode, CSystemControl* const pControl, uint32 const version)
{
	string const type = pNode->getAttr(CryAudio::s_szTypeAttribute);

	if (type.compare(CryAudio::s_szDataLoadType) == 0)
	{
		pControl->SetAutoLoad(true);
	}
	else
	{
		pControl->SetAutoLoad(false);
	}

	// Read the connection information for each of the platform groups
	std::vector<dll_string> const& platforms = GetIEditor()->GetConfigurationManager()->GetPlatformNames();
	int const numChildren = pNode->getChildCount();

	for (int i = 0; i < numChildren; ++i)
	{
		// Skip unused data from previous format
		XmlNodeRef const pPlatformNode = pNode->getChild(i);
		string const tag = pPlatformNode->getTag();

		if (tag == CryAudio::s_szPlatformTag)
		{
			// Get the index for that platform name
			int platformIndex = -1;
			string const platformName = pPlatformNode->getAttr(CryAudio::s_szNameAttribute);
			size_t const size = platforms.size();

			for (size_t j = 0; j < size; ++j)
			{
				if (strcmp(platformName.c_str(), platforms[j].c_str()) == 0)
				{
					platformIndex = j;
					break;
				}
			}

			if (platformIndex == -1)
			{
				m_errorCodeMask |= EErrorCode::UnkownPlatform;
				pControl->SetModified(true, true);
			}

			int const numConnections = pPlatformNode->getChildCount();

			for (int j = 0; j < numConnections; ++j)
			{
				XmlNodeRef const pConnectionNode = pPlatformNode->getChild(j);

				if (pConnectionNode != nullptr)
				{
					pControl->LoadConnectionFromXML(pConnectionNode, platformIndex);
				}
			}
		}
	}
}

//////////////////////////////////////////////////////////////////////////
void CFileLoader::LoadEditorData(XmlNodeRef const pEditorDataNode, CSystemAsset& library)
{
	int const size = pEditorDataNode->getChildCount();

	for (int i = 0; i < size; ++i)
	{
		XmlNodeRef const pChild = pEditorDataNode->getChild(i);

		if (pChild->isTag(s_szLibraryNodeTag))
		{
			LoadLibraryEditorData(pChild, library);
		}
		else if (pChild->isTag(s_szFoldersNodeTag))
		{
			LoadAllFolders(pChild, library);
		}
		else if (pChild->isTag(s_szControlsNodeTag))
		{
			LoadAllControlsEditorData(pChild);
		}
	}
}

//////////////////////////////////////////////////////////////////////////
void CFileLoader::LoadLibraryEditorData(XmlNodeRef const pLibraryNode, CSystemAsset& library)
{
	string description = "";
	pLibraryNode->getAttr(s_szDescriptionAttribute, description);

	if (!description.IsEmpty())
	{
		library.SetDescription(description);
	}
}

//////////////////////////////////////////////////////////////////////////
void CFileLoader::LoadAllFolders(XmlNodeRef const pFoldersNode, CSystemAsset& library)
{
	if (pFoldersNode != nullptr)
	{
		int const size = pFoldersNode->getChildCount();

		for (int i = 0; i < size; ++i)
		{
			LoadFolderData(pFoldersNode->getChild(i), library);
		}
	}
}

//////////////////////////////////////////////////////////////////////////
void CFileLoader::LoadFolderData(XmlNodeRef const pFolderNode, CSystemAsset& parentAsset)
{
	CSystemAsset* const pItem = AddUniqueFolderPath(&parentAsset, pFolderNode->getAttr(CryAudio::s_szNameAttribute));

	if (pItem != nullptr)
	{
		string description = "";
		pFolderNode->getAttr(s_szDescriptionAttribute, description);

		if (!description.IsEmpty())
		{
			pItem->SetDescription(description);
		}

		int const size = pFolderNode->getChildCount();

		for (int i = 0; i < size; ++i)
		{
			LoadFolderData(pFolderNode->getChild(i), *pItem);
		}
	}
}

//////////////////////////////////////////////////////////////////////////
void CFileLoader::LoadAllControlsEditorData(XmlNodeRef const pControlsNode)
{
	if (pControlsNode != nullptr)
	{
		int const size = pControlsNode->getChildCount();

		for (int i = 0; i < size; ++i)
		{
			LoadControlsEditorData(pControlsNode->getChild(i));
		}
	}
}

//////////////////////////////////////////////////////////////////////////
void CFileLoader::LoadControlsEditorData(XmlNodeRef const pParentNode)
{
	if (pParentNode != nullptr)
	{
		ESystemItemType const controlType = TagToType(pParentNode->getTag());
		string description = "";
		pParentNode->getAttr(s_szDescriptionAttribute, description);

		if ((controlType != ESystemItemType::Invalid) && !description.IsEmpty())
		{
			 CSystemControl* const pControl = m_assetsManager.FindControl(pParentNode->getAttr(CryAudio::s_szNameAttribute), controlType);

			if (pControl != nullptr)
			{
				pControl->SetDescription(description);
			}
		}
	}
}
} // namespace ACE