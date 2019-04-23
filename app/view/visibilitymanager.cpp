/*
*  Copyright 2016  Smith AR <audoban@openmailbox.org>
*                  Michail Vourlakos <mvourlakos@gmail.com>
*
*  This file is part of Latte-Dock
*
*  Latte-Dock is free software; you can redistribute it and/or
*  modify it under the terms of the GNU General Public License as
*  published by the Free Software Foundation; either version 2 of
*  the License, or (at your option) any later version.
*
*  Latte-Dock is distributed in the hope that it will be useful,
*  but WITHOUT ANY WARRANTY; without even the implied warranty of
*  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*  GNU General Public License for more details.
*
*  You should have received a copy of the GNU General Public License
*  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "visibilitymanager.h"

// local
#include "positioner.h"
#include "screenedgeghostwindow.h"
#include "view.h"
#include "../lattecorona.h"
#include "../layoutmanager.h"
#include "../screenpool.h"
#include "../wm/windowinfowrap.h"
#include "../../liblatte2/extras.h"

// Qt
#include <QDebug>

// KDE
#include <KWindowSystem>
#include <KWayland/Client/plasmashell.h>
#include <KWayland/Client/surface.h>

namespace Latte {
namespace ViewPart {

//! BEGIN: VisiblityManager implementation

VisibilityManager::VisibilityManager(PlasmaQuick::ContainmentView *view)
    : QObject(view)
{
    qDebug() << "VisibilityManager creating...";

    m_latteView = qobject_cast<Latte::View *>(view);
    m_corona = qobject_cast<Latte::Corona *>(view->corona());
    m_wm = m_corona->wm();

    if (m_latteView) {
        connect(m_latteView, &Latte::View::eventTriggered, this, &VisibilityManager::viewEventManager);

        connect(m_latteView, &Latte::View::absoluteGeometryChanged, this, [&]() {
            if (m_mode == Types::AlwaysVisible && m_latteView->screen()) {
                updateStrutsBasedOnLayoutsAndActivities();
            }
        });

        connect(m_latteView->windowsTracker(), &WindowsTracker::activeWindowDraggingStarted,
                this, &VisibilityManager::activeWindowDraggingStarted);
    }

    if (m_corona) {
        connect(this, &VisibilityManager::modeChanged, this, [&]() {
            emit m_corona->availableScreenRectChangedFrom(m_latteView);
        });
    }

    m_timerStartUp.setInterval(5000);
    m_timerStartUp.setSingleShot(true);
    m_timerShow.setSingleShot(true);
    m_timerHide.setSingleShot(true);

    connect(&m_timerShow, &QTimer::timeout, this, [&]() {
        if (m_isHidden) {
            //   qDebug() << "must be shown";
            emit mustBeShown();
        }
    });
    connect(&m_timerHide, &QTimer::timeout, this, [&]() {
        if (!m_blockHiding && !m_isHidden && !m_dragEnter) {
            //   qDebug() << "must be hide";
            emit mustBeHide();
        }
    });

    m_wm->setViewExtraFlags(*m_latteView);
    m_wm->addView(m_latteView->winId());

    restoreConfig();
}

VisibilityManager::~VisibilityManager()
{
    qDebug() << "VisibilityManager deleting...";
    m_wm->removeViewStruts(*m_latteView);
    m_wm->removeView(m_latteView->winId());

    if (m_edgeGhostWindow) {
        m_edgeGhostWindow->deleteLater();
    }
}

Types::Visibility VisibilityManager::mode() const
{
    return m_mode;
}

void VisibilityManager::setMode(Latte::Types::Visibility mode)
{
    if (m_mode == mode)
        return;

    Q_ASSERT_X(m_mode != Types::None, staticMetaObject.className(), "set visibility to Types::None");

    // clear mode
    for (auto &c : m_connections) {
        disconnect(c);
    }

    int base{0};

    if (m_mode == Types::AlwaysVisible) {
        //! remove struts for old always visible mode
        m_wm->removeViewStruts(*m_latteView);
    }

    m_timerShow.stop();
    m_timerHide.stop();
    m_mode = mode;

    if (mode != Types::AlwaysVisible && mode != Types::WindowsGoBelow) {
        //set wayland visibility mode
        if (m_latteView->surface()) {
            m_latteView->surface()->setPanelBehavior(KWayland::Client::PlasmaShellSurface::PanelBehavior::WindowsGoBelow);
        }

        m_connections[0] = connect(m_wm, &WindowSystem::currentDesktopChanged, this, [&] {
            if (m_raiseOnDesktopChange) {
                raiseViewTemporarily();
            }
        });
        m_connections[1] = connect(m_wm, &WindowSystem::currentActivityChanged, this, [&]() {
            if (m_raiseOnActivityChange) {
                raiseViewTemporarily();
            } else {
                updateHiddenState();
            }
        });

        base = 2;
    } else {
        //set wayland visibility mode
        if (m_latteView->surface()) {
            m_latteView->surface()->setPanelBehavior(KWayland::Client::PlasmaShellSurface::PanelBehavior::AutoHide);
        }
    }

    switch (m_mode) {
        case Types::AlwaysVisible: {
            if (m_latteView->containment() && m_latteView->screen()) {
                updateStrutsBasedOnLayoutsAndActivities();
            }

            if (m_corona && m_corona->layoutManager()->memoryUsage() == Types::MultipleLayouts) {
                m_connections[base] = connect(m_corona->activitiesConsumer(), &KActivities::Consumer::currentActivityChanged, this, [&]() {
                    updateStrutsBasedOnLayoutsAndActivities();
                });

                m_connections[base+1] = connect(m_latteView, &Latte::View::activitiesChanged, this, [&]() {
                    updateStrutsBasedOnLayoutsAndActivities();
                });
            }

            raiseView(true);
        }
        break;

        case Types::AutoHide: {
            m_connections[base] = connect(this, &VisibilityManager::containsMouseChanged, this, [&]() {
                raiseView(m_containsMouse);
            });

            raiseView(m_containsMouse);
        }
        break;

        case Types::DodgeActive: {
            m_connections[base] = connect(this, &VisibilityManager::containsMouseChanged
                                     , this, &VisibilityManager::dodgeActive);
            m_connections[base+1] = connect(m_latteView->windowsTracker(), &WindowsTracker::activeWindowTouchingChanged
                                     , this, &VisibilityManager::dodgeActive);

            dodgeActive();
        }
        break;

        case Types::DodgeMaximized: {
            m_connections[base] = connect(this, &VisibilityManager::containsMouseChanged
                                     , this, &VisibilityManager::dodgeMaximized);
            m_connections[base+1] = connect(m_latteView->windowsTracker(), &WindowsTracker::activeWindowMaximizedChanged
                                     , this, &VisibilityManager::dodgeActive);

            dodgeMaximized();
        }
        break;

        case Types::DodgeAllWindows: {
            m_connections[base] = connect(this, &VisibilityManager::containsMouseChanged
                                     , this, &VisibilityManager::dodgeAllWindows);

            m_connections[base+1] = connect(m_latteView->windowsTracker(), &WindowsTracker::existsWindowTouchingChanged
                                     , this, &VisibilityManager::dodgeAllWindows);
        }
        break;

        case Types::WindowsGoBelow:
        break;

        default:
        break;
    }

    m_latteView->containment()->config().writeEntry("visibility", static_cast<int>(m_mode));

    updateKWinEdgesSupport();

    emit modeChanged();
}

void VisibilityManager::updateStrutsBasedOnLayoutsAndActivities()
{
    bool multipleLayoutsAndCurrent = (m_corona->layoutManager()->memoryUsage() == Types::MultipleLayouts
                                      && m_latteView->managedLayout() && !m_latteView->positioner()->inLocationChangeAnimation()
                                      && m_latteView->managedLayout()->isCurrent());

    if (m_corona->layoutManager()->memoryUsage() == Types::SingleLayout || multipleLayoutsAndCurrent) {
        m_wm->setViewStruts(*m_latteView, m_latteView->absoluteGeometry(), m_latteView->location());
    } else {
        m_wm->removeViewStruts(*m_latteView);
    }
}

bool VisibilityManager::raiseOnDesktop() const
{
    return m_raiseOnDesktopChange;
}

void VisibilityManager::setRaiseOnDesktop(bool enable)
{
    if (enable == m_raiseOnDesktopChange)
        return;

    m_raiseOnDesktopChange = enable;
    emit raiseOnDesktopChanged();
}

bool VisibilityManager::raiseOnActivity() const
{
    return m_raiseOnActivityChange;
}

void VisibilityManager::setRaiseOnActivity(bool enable)
{
    if (enable == m_raiseOnActivityChange)
        return;

    m_raiseOnActivityChange = enable;
    emit raiseOnActivityChanged();
}

bool VisibilityManager::isHidden() const
{
    return m_isHidden;
}

void VisibilityManager::setIsHidden(bool isHidden)
{
    if (m_isHidden == isHidden)
        return;

    if (m_blockHiding && isHidden) {
        qWarning() << "isHidden property is blocked, ignoring update";
        return;
    }

    m_isHidden = isHidden;

    updateGhostWindowState();

    emit isHiddenChanged();
}

bool VisibilityManager::blockHiding() const
{
    return m_blockHiding;
}

void VisibilityManager::setBlockHiding(bool blockHiding)
{
    if (m_blockHiding == blockHiding) {
        return;
    }

    m_blockHiding = blockHiding;
    // qDebug() << "blockHiding:" << blockHiding;

    if (m_blockHiding) {
        m_timerHide.stop();

        if (m_isHidden) {
            emit mustBeShown();
        }
    } else {
        updateHiddenState();
    }

    emit blockHidingChanged();
}

int VisibilityManager::timerShow() const
{
    return m_timerShow.interval();
}

void VisibilityManager::setTimerShow(int msec)
{
    m_timerShow.setInterval(msec);
    emit timerShowChanged();
}

int VisibilityManager::timerHide() const
{
    return m_timerHide.interval();
}

void VisibilityManager::setTimerHide(int msec)
{
    m_timerHide.setInterval(msec);
    emit timerHideChanged();
}

bool VisibilityManager::supportsKWinEdges() const
{
    return (m_edgeGhostWindow != nullptr);
}

void VisibilityManager::updateGhostWindowState()
{
    if (supportsKWinEdges()) {
        bool inCurrentLayout = (m_corona->layoutManager()->memoryUsage() == Types::SingleLayout ||
                                (m_corona->layoutManager()->memoryUsage() == Types::MultipleLayouts
                                 && m_latteView->managedLayout() && !m_latteView->positioner()->inLocationChangeAnimation()
                                 && m_latteView->managedLayout()->name() == m_corona->layoutManager()->currentLayoutName()));

        if (inCurrentLayout) {
            m_wm->setEdgeStateFor(m_edgeGhostWindow, m_isHidden);
        } else {
            m_wm->setEdgeStateFor(m_edgeGhostWindow, false);
        }
    }
}

void VisibilityManager::hide()
{
    if (KWindowSystem::isPlatformX11()) {
        m_latteView->setVisible(false);
    }
}

void VisibilityManager::show()
{
    if (KWindowSystem::isPlatformX11()) {
        m_latteView->setVisible(true);
    }
}


void VisibilityManager::raiseView(bool raise)
{
    if (m_blockHiding)
        return;

    if (raise) {
        m_timerHide.stop();

        if (!m_timerShow.isActive()) {
            m_timerShow.start();
        }
    } else if (!m_dragEnter) {
        m_timerShow.stop();

        if (m_hideNow) {
            m_hideNow = false;
            emit mustBeHide();
        } else if (!m_timerHide.isActive()) {
            m_timerHide.start();
        }
    }
}

void VisibilityManager::raiseViewTemporarily()
{
    if (m_raiseTemporarily)
        return;

    m_raiseTemporarily = true;
    m_timerHide.stop();
    m_timerShow.stop();

    if (m_isHidden)
        emit mustBeShown();

    QTimer::singleShot(qBound(1800, 2 * m_timerHide.interval(), 3000), this, [&]() {
        m_raiseTemporarily = false;
        m_hideNow = true;
        updateHiddenState();
    });
}

void VisibilityManager::updateHiddenState()
{
    if (m_dragEnter)
        return;

    switch (m_mode) {
        case Types::AutoHide:
            raiseView(m_containsMouse);
            break;

        case Types::DodgeActive:
            dodgeActive();
            break;

        case Types::DodgeMaximized:
            dodgeMaximized();
            break;

        case Types::DodgeAllWindows:
            dodgeAllWindows();
            break;

        default:
            break;
    }
}

void VisibilityManager::applyActivitiesToHiddenWindows(const QStringList &activities)
{
    if (m_edgeGhostWindow) {
        m_wm->setWindowOnActivities(*m_edgeGhostWindow, activities);
    }
}

void VisibilityManager::activeWindowDraggingStarted()
{
    setContainsMouse(false);
    updateHiddenState();
}

void VisibilityManager::dodgeActive()
{
    if (m_raiseTemporarily)
        return;

    //!don't send false raiseView signal when containing mouse
    if (m_containsMouse) {
        raiseView(true);
        return;
    }

    raiseView(!m_latteView->windowsTracker()->activeWindowTouching());
}

void VisibilityManager::dodgeMaximized()
{
    if (m_raiseTemporarily)
        return;

    //!don't send false raiseView signal when containing mouse
    if (m_containsMouse) {
        raiseView(true);
        return;
    }

    raiseView(!m_latteView->windowsTracker()->activeWindowMaximized());
}

void VisibilityManager::dodgeAllWindows()
{
    if (m_raiseTemporarily)
        return;

    if (m_containsMouse) {
        raiseView(true);
    }

    bool windowIntersects{m_latteView->windowsTracker()->activeWindowTouching() || m_latteView->windowsTracker()->existsWindowTouching()};

    raiseView(!windowIntersects);
}

void VisibilityManager::saveConfig()
{
    if (!m_latteView->containment())
        return;

    auto config = m_latteView->containment()->config();

    config.writeEntry("enableKWinEdges", m_enableKWinEdgesFromUser);
    config.writeEntry("timerShow", m_timerShow.interval());
    config.writeEntry("timerHide", m_timerHide.interval());
    config.writeEntry("raiseOnDesktopChange", m_raiseOnDesktopChange);
    config.writeEntry("raiseOnActivityChange", m_raiseOnActivityChange);

    m_latteView->containment()->configNeedsSaving();
}

void VisibilityManager::restoreConfig()
{
    if (!m_latteView || !m_latteView->containment()){
        return;
    }

    auto config = m_latteView->containment()->config();
    m_timerShow.setInterval(config.readEntry("timerShow", 0));
    m_timerHide.setInterval(config.readEntry("timerHide", 700));
    emit timerShowChanged();
    emit timerHideChanged();

    m_enableKWinEdgesFromUser = config.readEntry("enableKWinEdges", true);
    emit enableKWinEdgesChanged();

    setRaiseOnDesktop(config.readEntry("raiseOnDesktopChange", false));
    setRaiseOnActivity(config.readEntry("raiseOnActivityChange", false));

    auto storedMode = static_cast<Types::Visibility>(m_latteView->containment()->config().readEntry("visibility", static_cast<int>(Types::DodgeActive)));

    if (storedMode == Types::AlwaysVisible) {
        qDebug() << "Loading visibility mode: Always Visible , on startup...";
        setMode(Types::AlwaysVisible);
    } else {
        connect(&m_timerStartUp, &QTimer::timeout, this, [&]() {
            auto fMode = static_cast<Types::Visibility>(m_latteView->containment()->config().readEntry("visibility", static_cast<int>(Types::DodgeActive)));
            qDebug() << "Loading visibility mode:" << fMode << " on startup...";
            setMode(fMode);
        });
        connect(m_latteView->containment(), &Plasma::Containment::userConfiguringChanged
        , this, [&](bool configuring) {
            if (configuring && m_timerStartUp.isActive())
                m_timerStartUp.start(100);
        });

        m_timerStartUp.start();
    }

    connect(m_latteView->containment(), &Plasma::Containment::userConfiguringChanged
    , this, [&](bool configuring) {
        if (!configuring) {
            saveConfig();
        }
    });
}

bool VisibilityManager::containsMouse() const
{
    return m_containsMouse;
}

void VisibilityManager::setContainsMouse(bool contains)
{
    if (m_containsMouse == contains) {
        return;
    }

    m_containsMouse = contains;
    emit containsMouseChanged();

    if (contains && m_mode != Types::AlwaysVisible) {
        raiseView(true);
    }
}

void VisibilityManager::viewEventManager(QEvent *ev)
{
    switch (ev->type()) {
        case QEvent::Enter:
            setContainsMouse(true);
            break;

        case QEvent::Leave:
            setContainsMouse(false);
            break;

        case QEvent::DragEnter:
            m_dragEnter = true;

            if (m_isHidden)
                emit mustBeShown();

            break;

        case QEvent::DragLeave:
        case QEvent::Drop:
            m_dragEnter = false;
            updateHiddenState();
            break;

        case QEvent::Show:
            m_wm->setViewExtraFlags(*m_latteView);
            break;

        default:
            break;
    }
}

//! KWin Edges Support functions
bool VisibilityManager::enableKWinEdges() const
{
    return m_enableKWinEdgesFromUser;
}

void VisibilityManager::setEnableKWinEdges(bool enable)
{
    if (m_enableKWinEdgesFromUser == enable) {
        return;
    }

    m_enableKWinEdgesFromUser = enable;

    emit enableKWinEdgesChanged();

    updateKWinEdgesSupport();
}

void VisibilityManager::updateKWinEdgesSupport()
{
    if (m_mode == Types::AutoHide
        || m_mode == Types::DodgeActive
        || m_mode == Types::DodgeAllWindows
        || m_mode == Types::DodgeMaximized) {
        if (m_enableKWinEdgesFromUser) {
            createEdgeGhostWindow();
        } else if (!m_enableKWinEdgesFromUser) {
            deleteEdgeGhostWindow();
        }
    } else if (m_mode == Types::AlwaysVisible
               || m_mode == Types::WindowsGoBelow) {
        deleteEdgeGhostWindow();
    }
}

void VisibilityManager::createEdgeGhostWindow()
{
    if (!m_edgeGhostWindow) {
        m_edgeGhostWindow = new ScreenEdgeGhostWindow(m_latteView);

        m_wm->setViewExtraFlags(*m_edgeGhostWindow);

        connect(m_edgeGhostWindow, &ScreenEdgeGhostWindow::containsMouseChanged, this, [ = ](bool contains) {
            if (contains) {
                raiseView(true);
            } else {
                m_timerShow.stop();
                updateGhostWindowState();
            }
        });

        m_connectionsKWinEdges[0] = connect(m_wm, &WindowSystem::currentActivityChanged,
        this, [&]() {
            bool inCurrentLayout = (m_corona->layoutManager()->memoryUsage() == Types::SingleLayout ||
                                    (m_corona->layoutManager()->memoryUsage() == Types::MultipleLayouts
                                     && m_latteView->managedLayout() && !m_latteView->positioner()->inLocationChangeAnimation()
                                     && m_latteView->managedLayout()->name() == m_corona->layoutManager()->currentLayoutName()));

            if (m_edgeGhostWindow) {
                if (inCurrentLayout) {
                    m_wm->setEdgeStateFor(m_edgeGhostWindow, m_isHidden);
                } else {
                    m_wm->setEdgeStateFor(m_edgeGhostWindow, false);
                }
            }
        });

        emit supportsKWinEdgesChanged();
    }
}

void VisibilityManager::deleteEdgeGhostWindow()
{
    if (m_edgeGhostWindow) {
        m_edgeGhostWindow->deleteLater();
        m_edgeGhostWindow = nullptr;

        for (auto &c : m_connectionsKWinEdges) {
            disconnect(c);
        }

        emit supportsKWinEdgesChanged();
    }
}

//! END: VisibilityManager implementation

}
}
