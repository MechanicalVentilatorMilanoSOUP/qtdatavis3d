/****************************************************************************
**
** Copyright (C) 2013 Digia Plc
** All rights reserved.
** For any questions to Digia, please use contact form at http://qt.digia.com
**
** This file is part of the QtDataVis3D module.
**
** Licensees holding valid Qt Enterprise licenses may use this file in
** accordance with the Qt Enterprise License Agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and Digia.
**
** If you have questions regarding the use of this file, please use
** contact form at http://qt.digia.com
**
****************************************************************************/

#include "qbardataproxy.h"
#include "qbardataproxy_p.h"
#include <QMutexLocker>

QT_DATAVIS3D_BEGIN_NAMESPACE

QBarDataProxy::QBarDataProxy() :
    QAbstractDataProxy(new QBarDataProxyPrivate(this))
{
}

QBarDataProxy::QBarDataProxy(QBarDataProxyPrivate *d) :
    QAbstractDataProxy(d)
{
}

QBarDataProxy::~QBarDataProxy()
{
}

void QBarDataProxy::resetArray(QBarDataArray *newArray)
{
    if (dptr()->resetArray(newArray))
        emit arrayReset();
}

void QBarDataProxy::setRow(int rowIndex, QBarDataRow *row)
{
    dptr()->setRow(rowIndex, row);
    emit rowsChanged(rowIndex, 1);
}

void QBarDataProxy::setRows(int rowIndex, QBarDataArray *rows)
{
    int count = rows->size();
    dptr()->setRows(rowIndex, rows);
    emit rowsChanged(rowIndex, count);
}

void QBarDataProxy::setItem(int rowIndex, int columnIndex, QBarDataItem *item)
{
    dptr()->setItem(rowIndex, columnIndex, item);
    emit itemChanged(rowIndex, columnIndex);
}

int QBarDataProxy::addRow(QBarDataRow *row)
{
    int addIndex = dptr()->addRow(row);
    emit rowsAdded(addIndex, 1);
    return addIndex;
}

int QBarDataProxy::addRows(QBarDataArray *rows)
{
    int addCount = rows->size();
    int addIndex = dptr()->addRows(rows); // deletes original rows array
    emit rowsAdded(addIndex, addCount);
    return addIndex;
}

void QBarDataProxy::insertRow(int rowIndex, QBarDataRow *row)
{
    dptr()->insertRow(rowIndex, row);
    emit rowsInserted(rowIndex, 1);
}

void QBarDataProxy::insertRows(int rowIndex, QBarDataArray *rows)
{
    int insertCount = rows->size();
    dptr()->insertRows(rowIndex, rows); // deletes original rows array
    emit rowsInserted(rowIndex, insertCount);
}

void QBarDataProxy::removeRows(int rowIndex, int removeCount)
{
    if (rowIndex < rowCount()) {
        dptr()->removeRows(rowIndex, removeCount);
        emit rowsRemoved(rowIndex, removeCount);
    }
}

// Mutexing data accessors should be done by user, if needed
int QBarDataProxy::rowCount() const
{
    return dptrc()->m_dataArray.size();
}

const QBarDataArray *QBarDataProxy::array() const
{
    return &dptrc()->m_dataArray;
}

const QBarDataRow *QBarDataProxy::rowAt(int rowIndex) const
{
    const QBarDataArray &dataArray = dptrc()->m_dataArray;
    Q_ASSERT(rowIndex >= 0 && rowIndex < dataArray.size());
    return dataArray[rowIndex];
}

const QBarDataItem *QBarDataProxy::itemAt(int rowIndex, int columnIndex) const
{
    const QBarDataArray &dataArray = dptrc()->m_dataArray;
    Q_ASSERT(rowIndex >= 0 && rowIndex < dataArray.size());
    const QBarDataRow &dataRow = *dataArray[rowIndex];
    Q_ASSERT(columnIndex >= 0 && columnIndex < dataRow.size());
    return &dataRow.at(columnIndex);
}

QBarDataProxyPrivate *QBarDataProxy::dptr()
{
    return static_cast<QBarDataProxyPrivate *>(d_ptr.data());
}

const QBarDataProxyPrivate *QBarDataProxy::dptrc() const
{
    return static_cast<const QBarDataProxyPrivate *>(d_ptr.data());
}

// QBarDataProxyPrivate

QBarDataProxyPrivate::QBarDataProxyPrivate(QBarDataProxy *q)
    : QAbstractDataProxyPrivate(q, QAbstractDataProxy::DataTypeBar)
{
}

QBarDataProxyPrivate::~QBarDataProxyPrivate()
{
    clearArray();
}

bool QBarDataProxyPrivate::resetArray(QBarDataArray *newArray)
{
    QMutexLocker locker(&m_mutex);

    if (!m_dataArray.size() && (!newArray || !newArray->size()))
        return false;

    clearArray();

    if (newArray) {
        m_dataArray = *newArray;
        delete newArray;
    }

    return true;
}

void QBarDataProxyPrivate::setRow(int rowIndex, QBarDataRow *row)
{
    QMutexLocker locker(&m_mutex);
    Q_ASSERT(rowIndex >= 0 && rowIndex < m_dataArray.size());
    clearRow(rowIndex);
    m_dataArray[rowIndex] = row;
}

void QBarDataProxyPrivate::setRows(int rowIndex, QBarDataArray *rows)
{
    QMutexLocker locker(&m_mutex);
    Q_ASSERT(rowIndex >= 0 && (rowIndex + rows->size()) <= m_dataArray.size());
    for (int i = 0; i < rows->size(); i++) {
        clearRow(rowIndex);
        m_dataArray[rowIndex] = rows->at(i);
        rowIndex++;
    }
    delete rows;
}

void QBarDataProxyPrivate::setItem(int rowIndex, int columnIndex, QBarDataItem *item)
{
    QMutexLocker locker(&m_mutex);
    Q_ASSERT(rowIndex >= 0 && rowIndex < m_dataArray.size());
    QBarDataRow &row = *m_dataArray[rowIndex];
    Q_ASSERT(columnIndex < row.size());
    row[columnIndex] = *item;
    delete item;
}

int QBarDataProxyPrivate::addRow(QBarDataRow *row)
{
    QMutexLocker locker(&m_mutex);
    int currentSize = m_dataArray.size();
    m_dataArray.append(row);
    return currentSize;
}

int QBarDataProxyPrivate::addRows(QBarDataArray *rows)
{
    QMutexLocker locker(&m_mutex);
    int currentSize = m_dataArray.size();
    for (int i = 0; i < rows->size(); i++)
        m_dataArray.append(rows->at(i));
    delete rows;
    return currentSize;
}

void QBarDataProxyPrivate::insertRow(int rowIndex, QBarDataRow *row)
{
    QMutexLocker locker(&m_mutex);
    Q_ASSERT(rowIndex >= 0 && rowIndex <= m_dataArray.size());
    m_dataArray.insert(rowIndex, row);
}

void QBarDataProxyPrivate::insertRows(int rowIndex, QBarDataArray *rows)
{
    QMutexLocker locker(&m_mutex);
    Q_ASSERT(rowIndex >= 0 && rowIndex <= m_dataArray.size());
    for (int i = 0; i < rows->size(); i++)
        m_dataArray.insert(rowIndex++, rows->at(i));
    delete rows;
}

void QBarDataProxyPrivate::removeRows(int rowIndex, int removeCount)
{
    QMutexLocker locker(&m_mutex);
    Q_ASSERT(rowIndex >= 0);
    int maxRemoveCount = m_dataArray.size() - rowIndex;
    removeCount = qMin(removeCount, maxRemoveCount);
    for (int i = 0; i < removeCount; i++) {
        clearRow(rowIndex);
        m_dataArray.removeAt(rowIndex);
    }
}

// Protected & private functions. Do not mutex as these are used from mutexed functions.

void QBarDataProxyPrivate::clearRow(int rowIndex)
{
    if (m_dataArray[rowIndex]) {
        delete m_dataArray[rowIndex];
        m_dataArray[rowIndex] = 0;
    }
}

void QBarDataProxyPrivate::clearArray()
{
    for (int i = 0; i < m_dataArray.size(); i++)
        clearRow(i);
    m_dataArray.clear();
}

QPair<GLfloat, GLfloat> QBarDataProxyPrivate::limitValues(int startRow, int endRow, int startColumn, int endColumn)
{
    QMutexLocker locker(&m_mutex);
    QPair<GLfloat, GLfloat> limits = qMakePair(0.0f, 0.0f);
    endRow = qMin(endRow, m_dataArray.size() - 1);
    for (int i = startRow; i <= endRow; i++) {
        QBarDataRow *row = m_dataArray.at(i);
        if (row) {
            endColumn = qMin(endColumn, row->size() - 1);
            for (int j = startColumn; j <= endColumn; j++) {
                const QBarDataItem &item = m_dataArray.at(i)->at(j);
                qreal itemValue = item.value();
                if (limits.second < itemValue)
                    limits.second = itemValue;
                if (limits.first > itemValue)
                    limits.first = itemValue;
            }
        }
    }
    return limits;
}

QT_DATAVIS3D_END_NAMESPACE
