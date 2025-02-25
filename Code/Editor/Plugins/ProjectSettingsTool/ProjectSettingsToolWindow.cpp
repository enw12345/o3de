/*
 * Copyright (c) Contributors to the Open 3D Engine Project. For complete copyright and license terms please see the LICENSE at the root of this distribution.
 * 
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */

#include "ProjectSettingsTool_precompiled.h"
#include "ProjectSettingsToolWindow.h"
#include "ui_ProjectSettingsToolWidget.h"

#include "DefaultImageValidator.h"
#include "PlatformSettings.h"
#include "ProjectSettingsContainer.h"
#include "ProjectSettingsValidator.h"
#include "PropertyImagePreview.h"
#include "PropertyFileSelect.h"
#include "PropertyLinked.h"
#include "Utils.h"
#include "ValidationHandler.h"

#include <AzCore/Component/ComponentApplicationBus.h>
#include <AzCore/IO/Path/Path.h>

#include <AzToolsFramework/UI/PropertyEditor/InstanceDataHierarchy.h>
#include <AzToolsFramework/UI/PropertyEditor/PropertyManagerComponent.h>
#include <AzToolsFramework/UI/PropertyEditor/ReflectedPropertyEditor.hxx>
#include <AzToolsFramework/UI/UICore/WidgetHelpers.h>

#include <QMessageBox>
#include <QCloseEvent>
#include <QScrollBar>
#include <QTimer>

// The object name in json for android
const static char* androidSettings = "android_settings";
static bool g_serializeRegistered = false;

namespace ProjectSettingsTool
{
    using XmlNode = AZ::rapidxml::xml_node<char>;

    ProjectSettingsToolWindow::ProjectSettingsToolWindow(QWidget* parent)
        : QWidget(parent)
        , LastPathBus::Handler()
        , m_ui(new Ui::ProjectSettingsToolWidget())
        , m_reconfigureProcess()
        , m_devRoot(GetDevRoot())
        , m_projectRoot(GetProjectRoot())
        , m_projectName(GetProjectName())
        , m_plistsInitVector(
            PlatformEnabled(PlatformId::Ios) ?
            ProjectSettingsContainer::PlistInitVector({
                ProjectSettingsContainer::PlatformAndPath
                { PlatformId::Ios, GetPlatformResource(PlatformId::Ios) }
                })
            :
                ProjectSettingsContainer::PlistInitVector())
        , m_settingsContainer(AZStd::make_unique<ProjectSettingsContainer>(m_projectRoot + "/project.json", m_plistsInitVector))
        , m_validator(AZStd::make_unique<Validator>())
        , m_platformProperties()
        , m_platformPropertyEditors()
        , m_propertyHandlers()
        , m_validationHandler(AZStd::make_unique<ValidationHandler>())
        , m_linkHandler(nullptr)
        // The default path to select images at
        , m_lastImagesPath(QStringLiteral("%1Code%2/Resources")
            .arg(m_projectRoot.c_str()
            , m_projectName.c_str()))
        , m_invalidState(false)
    {
        // Shows any and all errors that occurred during serialization with option to quit out on each one.
        ShowAllErrorsThenExitIfInvalid();

        if (!g_serializeRegistered)
        {
            ReflectPlatformClasses();
            g_serializeRegistered = true;
        }

        InitializeUi();
        RegisterHandlersAndBusses();
        AddAllPlatformsToUi();
        MakeSerializers();
        if (m_invalidState)
        {
            // Exit for safety
            return;
        }

        LoadPropertiesFromSettings();
        m_linkHandler->LinkAllProperties();

        // Hide the iOS tab if that platform is not enabled.
        if (!PlatformEnabled(PlatformId::Ios))
        {
            m_ui->platformTabs->removeTab(m_ui->platformTabs->indexOf(m_ui->iosTab));
        }
    }

    ProjectSettingsToolWindow::~ProjectSettingsToolWindow()
    {
        UnregisterHandlersAndBusses();
    }

    QString ProjectSettingsToolWindow::GetLastImagePath()
    {
        return m_lastImagesPath;
    }

    void ProjectSettingsToolWindow::SetLastImagePath(const QString& path)
    {
        m_lastImagesPath = path;
    }

    FunctorValidator* ProjectSettingsToolWindow::GetValidator(FunctorValidator::FunctorType functor)
    {
        return m_validator->GetQValidator(functor);
    }

    void ProjectSettingsToolWindow::TrackValidator(FunctorValidator* validator)
    {
        m_validator->TrackThisValidator(validator);
    }

    void ProjectSettingsToolWindow::ReflectPlatformClasses()
    {
        AZ::SerializeContext* context = nullptr;
        AZ::ComponentApplicationBus::BroadcastResult(context, &AZ::ComponentApplicationRequests::GetSerializeContext);

        BaseSettings::Reflect(context);
        AndroidSettings::Reflect(context);
        IosSettings::Reflect(context);
    }

    void ProjectSettingsToolWindow::RegisterHandlersAndBusses()
    {
        m_propertyHandlers.push_back(PropertyFuncValLineEditHandler::Register(m_validationHandler.get()));
        m_propertyHandlers.push_back(PropertyFuncValBrowseEditHandler::Register(m_validationHandler.get()));
        m_propertyHandlers.push_back(PropertyFileSelectHandler::Register(m_validationHandler.get()));
        m_propertyHandlers.push_back(PropertyImagePreviewHandler::Register(m_validationHandler.get()));
        m_linkHandler = PropertyLinkedHandler::Register(m_validationHandler.get());
        m_propertyHandlers.push_back(m_linkHandler);
        LastPathBus::Handler::BusConnect();
        ValidatorBus::Handler::BusConnect();
    }

    void ProjectSettingsToolWindow::UnregisterHandlersAndBusses()
    {
        ValidatorBus::Handler::BusDisconnect();
        LastPathBus::Handler::BusDisconnect();

        for (AzToolsFramework::PropertyHandlerBase* handler : m_propertyHandlers)
        {
            AzToolsFramework::PropertyTypeRegistrationMessages::Bus::Broadcast(
                &AzToolsFramework::PropertyTypeRegistrationMessages::Bus::Handler::UnregisterPropertyType,
                handler);
            delete handler;
        }
    }

    void ProjectSettingsToolWindow::MakeSerializerJson(const Platform& plat, AzToolsFramework::InstanceDataHierarchy& hierarchy, rapidjson::Document* doc)
    {
        m_platformSerializers[static_cast<unsigned>(plat.m_id)] = AZStd::make_unique<Serializer>(hierarchy.GetRoot(), doc);
    }

    void ProjectSettingsToolWindow::MakeSerializerJsonNonRoot(const Platform& plat, AzToolsFramework::InstanceDataHierarchy& hierarchy, rapidjson::Document* doc, rapidjson::Value* jsonRoot)
    {
        m_platformSerializers[static_cast<unsigned>(plat.m_id)] = AZStd::make_unique<Serializer>(hierarchy.GetRoot(), doc, jsonRoot);
    }

    void ProjectSettingsToolWindow::MakeSerializerPlist(const Platform& plat, AzToolsFramework::InstanceDataHierarchy& hierarchy, PlistDictionary* dict)
    {
        m_platformSerializers[static_cast<unsigned>(plat.m_id)] = AZStd::make_unique<Serializer>(hierarchy.GetRoot(), AZStd::unique_ptr<PlistDictionary>(dict));
    }

    void ProjectSettingsToolWindow::closeEvent(QCloseEvent* event)
    {
        if (!m_invalidState)
        {
            // Check if ui is loaded
            if (m_ui->saveButton != nullptr)
            {
                // Save button is used as an inverse bool to tell if configure is being run or settings are being saved
                if (m_ui->saveButton->isEnabled())
                {
                    if (!UiEqualToSettings())
                    {
                        int result = QMessageBox::question
                        (
                            this,
                            tr("Warning"),
                            tr("There are currently unsaved changes. Are you sure you want to cancel?"),
                            QMessageBox::Yes,
                            QMessageBox::No
                        );


                        if (QMessageBox::Yes == result)
                        {
                            QWidget::closeEvent(event);
                        }
                        else
                        {
                            event->setAccepted(false);
                        }
                    }
                    else
                    {
                        QWidget::closeEvent(event);
                    }
                }
                else
                {
                    QMessageBox::information
                    (
                        this,
                        tr("Info"),
                        tr("Cannot close until settings have been reconfigured."),
                        QMessageBox::Ok
                    );

                    event->setAccepted(false);
                }
            }
            else
            {
                QWidget::closeEvent(event);
            }
        }
        else
        {
            QWidget::closeEvent(event);
        }
    }

    void ProjectSettingsToolWindow::ForceClose()
    {
        m_invalidState = true;
        // Potentially called from the constructor, when the widget/window aren't properly set up, so delay this until after it's all setup
        QTimer::singleShot(0, this, [this]() {window()->close();} );
    }

    bool ProjectSettingsToolWindow::IfErrorShowThenExit()
    {
        // Grabs the earliest unseen error popping it off the error queue
        AZ::Outcome<void, SettingsError> error = m_settingsContainer->GetError();
        if (!error.IsSuccess())
        {
            bool shouldAbort = error.GetError().m_error == m_settingsContainer->GetFailedLoadingPlistText();
            QMessageBox::StandardButton result = QMessageBox::critical
            (
                this,
                error.GetError().m_error.c_str(),
                error.GetError().m_reason.c_str(),
                shouldAbort ? QMessageBox::Abort : QMessageBox::StandardButtons(QMessageBox::Ok | QMessageBox::Abort),
                shouldAbort ? QMessageBox::Abort : QMessageBox::Ok
            );
            if (result == QMessageBox::Abort)
            {
                ForceClose();
            }
            return true;
        }
        return false;
    }

    void ProjectSettingsToolWindow::ShowAllErrorsThenExitIfInvalid()
    {
        while (IfErrorShowThenExit())
        {
            if (m_invalidState)
            {
                // Exit for safety
                return;
            }
        }
    }

    void ProjectSettingsToolWindow::InitializeUi()
    {
        // setup
        m_ui->setupUi(this);

        AzQtComponents::TabWidget::applySecondaryStyle(m_ui->platformTabs, false);

        ResizeTabs(m_ui->platformTabs->currentIndex());

        m_ui->reconfigureLog->hide();

        // connects
        connect
        (
            &m_reconfigureProcess,
            static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
            this,
            [this]()
            {
                m_ui->saveButton->setEnabled(true);
                m_ui->reloadButton->setEnabled(true);
                m_ui->reconfigureLog->insertPlainText(tr("\n Reconfiguration Finished"));
                QScrollBar* scrollbar = m_ui->reconfigureLog->verticalScrollBar();
                scrollbar->setValue(scrollbar->maximum());
            }
        );

        connect(&m_reconfigureProcess, &QProcess::readyReadStandardOutput, this,
            [this]()
            {
                m_ui->reconfigureLog->insertPlainText(m_reconfigureProcess.readAllStandardOutput());
                if (!m_ui->reconfigureLog->textCursor().hasSelection())
                {
                    QScrollBar* scrollbar = m_ui->reconfigureLog->verticalScrollBar();
                    scrollbar->setValue(scrollbar->maximum());
                }
            });
        connect(&m_reconfigureProcess, &QProcess::readyReadStandardError, this,
            [this]()
            {
                m_ui->reconfigureLog->insertPlainText(m_reconfigureProcess.readAllStandardError());
                if (!m_ui->reconfigureLog->textCursor().hasSelection())
                {
                    QScrollBar* scrollbar = m_ui->reconfigureLog->verticalScrollBar();
                    scrollbar->setValue(scrollbar->maximum());
                }
            });

        connect(m_ui->platformTabs, &QTabWidget::currentChanged,
            this, &ProjectSettingsToolWindow::ResizeTabs);

        connect(m_ui->saveButton, &QPushButton::clicked, this,
            &ProjectSettingsToolWindow::SaveSettingsFromUi);
        connect(m_ui->reloadButton, &QPushButton::clicked, this,
            &ProjectSettingsToolWindow::ReloadUiFromSettings);
    }

    void ProjectSettingsToolWindow::ResizeTabs(int index)
    {
        for (int i = 0; i < m_ui->platformTabs->count(); i++)
        {
            if (i != index)
            {
                m_ui->platformTabs->widget(i)->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
            }
        }

        // resize for current tab
        m_ui->platformTabs->widget(index)->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
        m_ui->platformTabs->widget(index)->resize(m_ui->platformTabs->widget(index)->minimumSizeHint());
        m_ui->platformTabs->widget(index)->adjustSize();
    }

    void ProjectSettingsToolWindow::AddAllPlatformsToUi()
    {
        for (int plat = 0; plat < static_cast<unsigned long long>(PlatformId::NumPlatformIds); ++plat)
        {
            AddPlatformToUi(Platforms[plat]);
        }
    }

    void ProjectSettingsToolWindow::AddPlatformToUi(const Platform& plat)
    {
        AZ::SerializeContext* context = nullptr;
        AZ::ComponentApplicationBus::BroadcastResult(context, &AZ::ComponentApplicationRequests::GetSerializeContext);

        QWidget* parent = nullptr;
        void* dataForPropertyEditor = nullptr;
        AZ::TypeId dataTypeID;
        switch (plat.m_id)
        {
        case PlatformId::Base:
            parent = m_ui->baseSettingsGroupBox;
            dataForPropertyEditor = &m_platformProperties.base;
            dataTypeID = m_platformProperties.base.TYPEINFO_Uuid();
            break;
        case PlatformId::Android:
            parent = m_ui->androidTab;
            dataForPropertyEditor = &m_platformProperties.android;
            dataTypeID = m_platformProperties.android.TYPEINFO_Uuid();
            break;
        case PlatformId::Ios:
            parent = m_ui->iosTab;
            dataForPropertyEditor = &m_platformProperties.ios;
            dataTypeID = m_platformProperties.ios.TYPEINFO_Uuid();
            break;
        default:
            AZ_Assert(false, "Cannot add unknown platform to ui.");
        }

        unsigned platIdValue = static_cast<unsigned>(plat.m_id);

        m_platformPropertyEditors[platIdValue] = aznew AzToolsFramework::ReflectedPropertyEditor(parent);
        parent->layout()->addWidget(m_platformPropertyEditors[platIdValue]);

        m_platformPropertyEditors[platIdValue]->Setup(context, nullptr, false);
        m_platformPropertyEditors[platIdValue]->AddInstance(dataForPropertyEditor, dataTypeID);
        m_platformPropertyEditors[platIdValue]->setVisible(true);
        m_platformPropertyEditors[platIdValue]->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
        m_platformPropertyEditors[platIdValue]->SetHideRootProperties(false);
        m_platformPropertyEditors[platIdValue]->SetDynamicEditDataProvider(nullptr);
        m_platformPropertyEditors[platIdValue]->ExpandAll();
        m_platformPropertyEditors[platIdValue]->InvalidateAll();
    }

    const char* GetPlatformKey(const Platform& plat)
    {
        switch (plat.m_id)
        {
        case PlatformId::Android:
            return androidSettings;
        default:
            return "";
        }
    }

    void ProjectSettingsToolWindow::MakeSerializers()
    {
        for (int plat = 0; plat < static_cast<unsigned long long>(PlatformId::NumPlatformIds); ++plat)
        {
            if (PlatformEnabled(static_cast<PlatformId>(plat)))
            {
                MakePlatformSerializer(Platforms[plat]);
            }
        }
    }

    void ProjectSettingsToolWindow::MakePlatformSerializer(const Platform& plat)
    {
        unsigned platIdValue = static_cast<unsigned>(plat.m_id);

        switch (plat.m_id)
        {
        case PlatformId::Base:
            m_platformPropertyEditors[platIdValue]->EnumerateInstances(AZStd::bind
                (
                    &ProjectSettingsToolWindow::MakeSerializerJson,
                    this,
                    plat,
                    AZStd::placeholders::_1,
                    &m_settingsContainer->GetProjectJsonDocument()
                ));
            break;
        case PlatformId::Android:
            m_platformPropertyEditors[platIdValue]->EnumerateInstances(AZStd::bind
                (
                    &ProjectSettingsToolWindow::MakeSerializerJsonNonRoot,
                    this,
                    plat,
                    AZStd::placeholders::_1,
                    &m_settingsContainer->GetProjectJsonDocument(),
                    m_settingsContainer->GetProjectJsonValue(GetPlatformKey(plat)).GetValue()
                ));
            break;
        case PlatformId::Ios:
        {
            PlistDictionary* dict = m_settingsContainer->GetPlistDictionary(plat).release();
            if (dict == nullptr)
            {
                QMessageBox::critical
                (
                    this,
                    "Critical",
                    "Ios pList is invalid. Project Settings Tool must close.",
                    QMessageBox::Abort
                );
                ForceClose();
            }

            m_platformPropertyEditors[platIdValue]->EnumerateInstances(AZStd::bind
            (
                &ProjectSettingsToolWindow::MakeSerializerPlist,
                this,
                plat,
                AZStd::placeholders::_1,
                // All arguments must be copy constructible so this must be released
                dict
            ));
            break;
        }
        default:
            AZ_Assert(false, "Cannot make serializer for unknown platform.");
            break;
        }
    }

    void ProjectSettingsToolWindow::LoadPropertiesFromSettings()
    {
        // Disable all fields links

        for (int plat = 0; plat < static_cast<unsigned long long>(PlatformId::NumPlatformIds); ++plat)
        {
            if (PlatformEnabled(static_cast<PlatformId>(plat)))
            {
                LoadPropertiesFromPlatformSettings(Platforms[plat]);
            }
        }
    }

    void ProjectSettingsToolWindow::LoadPropertiesFromPlatformSettings(const Platform& plat)
    {
        unsigned platIdValue = static_cast<unsigned>(plat.m_id);
        m_platformSerializers[platIdValue]->LoadFromSettings();
        m_platformPropertyEditors[platIdValue]->InvalidateValues();
    }

    bool ProjectSettingsToolWindow::UiEqualToSettings()
    {
        for (int plat = 0; plat < static_cast<unsigned long long>(PlatformId::NumPlatformIds); ++plat)
        {
            if (PlatformEnabled(static_cast<PlatformId>(plat)))
            {
                if (!UiEqualToPlatformSettings(Platforms[plat]))
                {
                    return false;
                }
            }
        }

        return true;
    }

    bool ProjectSettingsToolWindow::UiEqualToPlatformSettings(const Platform& plat)
    {
        return m_platformSerializers[static_cast<unsigned>(plat.m_id)]->UiEqualToSettings();
    }

    bool ProjectSettingsToolWindow::ValidateAllProperties()
    {
        return m_validationHandler->AllValid();
    }

    void ProjectSettingsToolWindow::SaveSettingsFromUi()
    {
        bool anySaves = false;
        const unsigned long long numPlatforms = static_cast<unsigned long long>(PlatformId::NumPlatformIds);
        bool needToSavePlat[numPlatforms] = {false};

        for (int plat = 0; plat < numPlatforms; ++plat)
        {
            if (PlatformEnabled(static_cast<PlatformId>(plat)))
            {
                const Platform& platform = Platforms[plat];
                if (!UiEqualToPlatformSettings(platform))
                {
                    needToSavePlat[plat] = true;
                    anySaves = true;
                }
            }
        }

        if (anySaves)
        {
            // Keeps queued button presses from getting in
            if (m_ui->saveButton->isEnabled())
            {
                m_ui->saveButton->setEnabled(false);
                m_ui->reloadButton->setEnabled(false);

                if (ValidateAllProperties())
                {
                    bool projectJsonChanged = false;

                    for (int plat = 0; plat < numPlatforms; ++plat)
                    {
                        const Platform& platform = Platforms[plat];

                        if (needToSavePlat[plat])
                        {
                            m_platformSerializers[plat]->SaveToSettings();
                            if (m_settingsContainer->IsPlistPlatform(platform))
                            {
                                m_settingsContainer->SavePlistData(platform);
                            }
                            else
                            {
                                projectJsonChanged = true;
                            }
                        }
                    }
                    if (projectJsonChanged)
                    {
                        m_settingsContainer->SaveProjectJsonData();
                    }

                    ShowAllErrorsThenExitIfInvalid();

                    m_ui->reconfigureLog->setText("");
                    QMessageBox::information(this, tr("Project Settings Saved"),
                        tr("The project may need to be manually reconfigured for the new settings to be applied."));

                    m_ui->reloadButton->setEnabled(true);
                    m_ui->saveButton->setEnabled(true);
                }
                // Show a message box telling user settings failed to save
                else
                {
                    QMessageBox::critical(this, tr("Failed To Save"), tr("Failed to save due to invalid settings."));
                    m_ui->reloadButton->setEnabled(true);
                    m_ui->saveButton->setEnabled(true);
                }
            }
        }
    }

    void ProjectSettingsToolWindow::SaveSettingsFromPlatformUi(const Platform& plat)
    {
        m_platformSerializers[static_cast<unsigned>(plat.m_id)]->SaveToSettings();
        m_settingsContainer->SavePlatformData(plat);
        ShowAllErrorsThenExitIfInvalid();
    }

    void ProjectSettingsToolWindow::ReloadUiFromSettings()
    {
        if (!UiEqualToSettings())
        {
            int result = QMessageBox::warning
                (
                this,
                tr("Reload Settings"),
                tr("Are you sure you would like to reload settings from file? All changes will be lost."),
                QMessageBox::Reset,
                QMessageBox::Cancel);

            if (result == QMessageBox::Reset)
            {
                m_settingsContainer->ReloadProjectJsonData();
                m_settingsContainer->ReloadPlistData();
                MakeSerializers();

                // Disable links to avoid overwriting values while loading
                m_linkHandler->DisableAllPropertyLinks();
                LoadPropertiesFromSettings();

                // Re-enable them then mirror
                m_linkHandler->EnableAllPropertyLinks();
                m_linkHandler->EnableOptionalLinksIfAllPropertiesEqual();
                m_linkHandler->MirrorAllLinkedProperties();

                // Mark any invalid fields loaded from file
                ValidateAllProperties();
            }
        }
    }

    bool ProjectSettingsToolWindow::PlatformEnabled(PlatformId platformId)
    {
        // iOS can be disabled if the plist file is missing
        if (platformId == PlatformId::Ios)
        {
            AZStd::string plistPath = GetPlatformResource(platformId);
            return !plistPath.empty();
        }

        return true;
    }

    AZStd::string ProjectSettingsToolWindow::GetPlatformResource(PlatformId platformId)
    {
        if (platformId == PlatformId::Ios)
        {
            const char* searchPaths[] = {
                "Resources/Platform/iOS/Info.plist",

                // legacy paths
                "Gem/Resources/Platform/iOS/Info.plist",
                "Gem/Resources/IOSLauncher/Info.plist",
            };

            for (auto relPath : searchPaths)
            {
                AZ::IO::FixedMaxPath projectPlist{ m_projectRoot };
                projectPlist /= relPath;

                if (AZ::IO::SystemFile::Exists(projectPlist.c_str()))
                {
                    return projectPlist.LexicallyNormal().String();
                }
            }
        }

        return AZStd::string();
    }

#include <moc_ProjectSettingsToolWindow.cpp>
} // namespace ProjectSettingsTool
