/*
 * Copyright 2021  Michail Vourlakos <mvourlakos@gmail.com>
 *
 * This file is part of Latte-Dock
 *
 * Latte-Dock is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * Latte-Dock is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "appletsmodel.h"

// local
#include "../../layout/abstractlayout.h"

// Qt
#include <QFont>
#include <QIcon>

// KDE
#include <KLocalizedString>

namespace Latte {
namespace Settings {
namespace Model {

Applets::Applets(QObject *parent)
    : QAbstractTableModel(parent)
{
    m_appletsWithNoPersonalData = {
        "org.kde.latte.separator",
        "org.kde.latte.spacer",
        "org.kde.latte.plasmoid",
        "org.kde.windowtitle",
        "org.kde.windowbuttons",
        "org.kde.windowappmenu"
    };
}

Applets::~Applets()
{
}

bool Applets::dataAreChanged() const
{
    return c_applets != o_applets;
}

int Applets::rowCount() const
{
    return c_applets.rowCount();
}

int Applets::rowCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);

    return c_applets.rowCount();
}

int Applets::columnCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);

    return 1;
}

int Applets::row(const QString &id)
{
    for (int i=0; i<c_applets.rowCount(); ++i){
        if (c_applets[i].id == id) {
            return i;
        }
    }

    return -1;
}

void Applets::initDefaults()
{
    for(int i=0; i<c_applets.rowCount(); ++i) {
        c_applets[i].isSelected = m_appletsWithNoPersonalData.contains(c_applets[i].id);
    }
}

void Applets::clear()
{
    if (c_applets.rowCount() > 0) {
        beginRemoveRows(QModelIndex(), 0, c_applets.rowCount() - 1);
        c_applets.clear();
        endRemoveRows();
    }
}

void Applets::reset()
{
    c_applets = o_applets;

    QVector<int> roles;
    roles << Qt::CheckStateRole;
    emit dataChanged(index(0, NAMECOLUMN), index(c_applets.rowCount()-1, NAMECOLUMN), roles);
}

void Applets::setData(const Latte::Data::AppletsTable &applets)
{
    clear();

    if (applets.rowCount() > 0) {
        beginInsertRows(QModelIndex(), 0, applets.rowCount()-1);
        c_applets = applets;
        initDefaults();
        o_applets = c_applets;
        endInsertRows();
    }
}

void Applets::selectAll()
{
    QVector<int> roles;
    roles << Qt::CheckStateRole;

    for(int i=0; i<c_applets.rowCount(); ++i) {
        if (!c_applets[i].isSelected) {
            c_applets[i].isSelected = true;
            emit dataChanged(index(i, NAMECOLUMN), index(i, NAMECOLUMN), roles);
        }
    }
}

void Applets::deselectAll()
{
    QVector<int> roles;
    roles << Qt::CheckStateRole;

    for(int i=0; i<c_applets.rowCount(); ++i) {
        if (c_applets[i].isSelected) {
            c_applets[i].isSelected = false;
            emit dataChanged(index(i, NAMECOLUMN), index(i, NAMECOLUMN), roles);
        }
    }
}

void Applets::setSelected(const Latte::Data::AppletsTable &applets)
{
    for(int i=0; i<applets.rowCount(); ++i) {
        int pos = c_applets.indexOf(applets[i].id);

        if (pos>=0 && applets[i].isSelected != c_applets[pos].isSelected) {
            QVector<int> roles;
            roles << Qt::CheckStateRole;

            c_applets[pos].isSelected = applets[i].isSelected;
            emit dataChanged(index(pos, NAMECOLUMN), index(pos, NAMECOLUMN), roles);
        }
    }
}

Qt::ItemFlags Applets::flags(const QModelIndex &index) const
{
    const int column = index.column();
    const int row = index.row();

    auto flags = QAbstractTableModel::flags(index);

    flags |= Qt::ItemIsUserCheckable;

    return flags;
}

QVariant Applets::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal) {
        return QAbstractTableModel::headerData(section, orientation, role);
    }

    if (role == Qt::FontRole) {
        QFont font = qvariant_cast<QFont>(QAbstractTableModel::headerData(section, orientation, role));
        font.setBold(true);
        return font;
    }

    switch(section) {
    case NAMECOLUMN:
        if (role == Qt::DisplayRole) {
            return QString(i18nc("column for current applets", "Current Applets"));
        }
        break;
    default:
        break;
    };

    return QAbstractTableModel::headerData(section, orientation, role);
}

bool Applets::setData(const QModelIndex &index, const QVariant &value, int role)
{
    const int row = index.row();
    const int column = index.column();

    if (!c_applets.rowExists(row) || column<0 || column > NAMECOLUMN) {
        return false;
    }

    //! specific roles to each independent cell
    switch (column) {
    case NAMECOLUMN:
        if (role == Qt::CheckStateRole) {
            c_applets[row].isSelected = (value.toInt() > 0 ? true : false);
            return true;
        }
        break;
    };

    return false;
}

QVariant Applets::data(const QModelIndex &index, int role) const
{
    const int row = index.row();
    int column = index.column();

    if (row >= rowCount()) {
        return QVariant{};
    }

    if (role == NAMEROLE || role == Qt::DisplayRole) {
        return c_applets[row].name;
    } else if (role == Qt::CheckStateRole) {
        return (c_applets[row].isSelected ? Qt::Checked : Qt::Unchecked);
    } else if (role== Qt::DecorationRole) {
        return QIcon::fromTheme(c_applets[row].icon);
    } else if (role == IDROLE) {
            return c_applets[row].id;
    } else if (role == SELECTEDROLE) {
        return c_applets[row].isSelected;
    } else if (role == ICONROLE) {
        return c_applets[row].icon;
    } else if (role == DESCRIPTIONROLE) {
        return c_applets[row].description;
    } else if (role == SORTINGROLE) {
        return c_applets[row].isInstalled() ? QString::number(1000) + c_applets[row].name : QString::number(0000) + c_applets[row].name;
    }

    return QVariant{};
}

}
}
}
