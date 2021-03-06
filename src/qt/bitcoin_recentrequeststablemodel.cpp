// Copyright (c) 2011-2014 The Bitcoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "bitcoin_recentrequeststablemodel.h"

#include "bitcoinunits.h"
#include "guiutil.h"
#include "optionsmodel.h"

Bitcoin_RecentRequestsTableModel::Bitcoin_RecentRequestsTableModel(Bitcoin_CWallet *wallet, Bitcoin_WalletModel *parent) :
    walletModel(parent)
{
    Q_UNUSED(wallet);
    nReceiveRequestsMaxId = 0;

    // Load entries from wallet
    std::vector<std::string> vReceiveRequests;
    parent->loadReceiveRequests(vReceiveRequests);
    BOOST_FOREACH(const std::string& request, vReceiveRequests)
        addNewRequest(request);

    /* These columns must match the indices in the ColumnIndex enumeration */
    columns << tr("Date") << tr("Label") << tr("Message") << tr("Amount");
}

Bitcoin_RecentRequestsTableModel::~Bitcoin_RecentRequestsTableModel()
{
    /* Intentionally left empty */
}

int Bitcoin_RecentRequestsTableModel::rowCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);

    return list.length();
}

int Bitcoin_RecentRequestsTableModel::columnCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);

    return columns.length();
}

QVariant Bitcoin_RecentRequestsTableModel::data(const QModelIndex &index, int role) const
{
    if(!index.isValid() || index.row() >= list.length())
        return QVariant();

    const Bitcoin_RecentRequestEntry *rec = &list[index.row()];

    if(role == Qt::DisplayRole || role == Qt::EditRole)
    {
        switch(index.column())
        {
        case Date:
            return GUIUtil::dateTimeStr(rec->date);
        case Label:
            if(rec->recipient.label.isEmpty() && role == Qt::DisplayRole)
            {
                return tr("(no label)");
            }
            else
            {
                return rec->recipient.label;
            }
        case Message:
            if(rec->recipient.message.isEmpty() && role == Qt::DisplayRole)
            {
                return tr("(no message)");
            }
            else
            {
                return rec->recipient.message;
            }
        case Amount:
            if (rec->recipient.amount == 0 && role == Qt::DisplayRole)
                return tr("(no amount)");
            else
                return BitcoinUnits::format(walletModel->getOptionsModel()->getDisplayUnit(), rec->recipient.amount);
        }
    }
    return QVariant();
}

bool Bitcoin_RecentRequestsTableModel::setData(const QModelIndex &index, const QVariant &value, int role)
{
    return true;
}

QVariant Bitcoin_RecentRequestsTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if(orientation == Qt::Horizontal)
    {
        if(role == Qt::DisplayRole && section < columns.size())
        {
            return columns[section];
        }
    }
    return QVariant();
}

QModelIndex Bitcoin_RecentRequestsTableModel::index(int row, int column, const QModelIndex &parent) const
{
    Q_UNUSED(parent);

    return createIndex(row, column);
}

bool Bitcoin_RecentRequestsTableModel::removeRows(int row, int count, const QModelIndex &parent)
{
    Q_UNUSED(parent);

    if(count > 0 && row >= 0 && (row+count) <= list.size())
    {
        const Bitcoin_RecentRequestEntry *rec;
        for (int i = 0; i < count; ++i)
        {
            rec = &list[row+i];
            if (!walletModel->saveReceiveRequest(rec->recipient.address.toStdString(), rec->id, ""))
                return false;
        }

        beginRemoveRows(parent, row, row + count - 1);
        list.erase(list.begin() + row, list.begin() + row + count);
        endRemoveRows();
        return true;
    } else {
        return false;
    }
}

Qt::ItemFlags Bitcoin_RecentRequestsTableModel::flags(const QModelIndex &index) const
{
    return Qt::ItemIsSelectable | Qt::ItemIsEnabled;
}

// called when adding a request from the GUI
void Bitcoin_RecentRequestsTableModel::addNewRequest(const Bitcoin_SendCoinsRecipient &recipient)
{
    Bitcoin_RecentRequestEntry newEntry;
    newEntry.id = ++nReceiveRequestsMaxId;
    newEntry.date = QDateTime::currentDateTime();
    newEntry.recipient = recipient;

    CDataStream ss(SER_DISK, Bitcoin_Params().ClientVersion());
    ss << newEntry;

    if (!walletModel->saveReceiveRequest(recipient.address.toStdString(), newEntry.id, ss.str()))
        return;

    addNewRequest(newEntry);
}

// called from ctor when loading from wallet
void Bitcoin_RecentRequestsTableModel::addNewRequest(const std::string &recipient)
{
    std::vector<char> data(recipient.begin(), recipient.end());
    CDataStream ss(data, SER_DISK, Bitcoin_Params().ClientVersion());

    Bitcoin_RecentRequestEntry entry;
    ss >> entry;

    if (entry.id == 0) // should not happen
        return;

    if (entry.id > nReceiveRequestsMaxId)
        nReceiveRequestsMaxId = entry.id;

    addNewRequest(entry);
}

// actually add to table in GUI
void Bitcoin_RecentRequestsTableModel::addNewRequest(Bitcoin_RecentRequestEntry &recipient)
{
    beginInsertRows(QModelIndex(), 0, 0);
    list.prepend(recipient);
    endInsertRows();
}

void Bitcoin_RecentRequestsTableModel::sort(int column, Qt::SortOrder order)
{
    qSort(list.begin(), list.end(), Bitcoin_RecentRequestEntryLessThan(column, order));
    emit dataChanged(index(0, 0, QModelIndex()), index(list.size() - 1, NUMBER_OF_COLUMNS - 1, QModelIndex()));
}

bool Bitcoin_RecentRequestEntryLessThan::operator()(Bitcoin_RecentRequestEntry &left, Bitcoin_RecentRequestEntry &right) const
{
    Bitcoin_RecentRequestEntry *pLeft = &left;
    Bitcoin_RecentRequestEntry *pRight = &right;
    if (order == Qt::DescendingOrder)
        std::swap(pLeft, pRight);

    switch(column)
    {
    case Bitcoin_RecentRequestsTableModel::Date:
        return pLeft->date.toTime_t() < pRight->date.toTime_t();
    case Bitcoin_RecentRequestsTableModel::Label:
        return pLeft->recipient.label < pRight->recipient.label;
    case Bitcoin_RecentRequestsTableModel::Message:
        return pLeft->recipient.message < pRight->recipient.message;
    case Bitcoin_RecentRequestsTableModel::Amount:
        return pLeft->recipient.amount < pRight->recipient.amount;
    default:
        return pLeft->id < pRight->id;
    }
}
