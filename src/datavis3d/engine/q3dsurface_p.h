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

//
//  W A R N I N G
//  -------------
//
// This file is not part of the QtDataVis3D API.  It exists purely as an
// implementation detail.  This header file may change from version to
// version without notice, or even be removed.
//
// We mean it.

#ifndef Q3DSURFACE_P_H
#define Q3DSURFACE_P_H

#include "surface3dcontroller_p.h"
#include "qdatavis3dnamespace.h"

#include <QList>

QT_DATAVIS3D_BEGIN_NAMESPACE

class Q3DSurface;

class Q3DSurfacePrivate : public QObject
{
public:
    Q3DSurfacePrivate(Q3DSurface *q, QRect rect);
    ~Q3DSurfacePrivate();

    // TODO
    void appendSeries(QList<qreal> series);
    QList<qreal> seriesAt(int i);
    int numOfSeries();
    // TODO

    Surface3dController *m_shared;

private:
    Q3DSurface *q_ptr;
    QList<QList<qreal> > m_seriesList; // Temp to be replaced by dataset
};

QT_DATAVIS3D_END_NAMESPACE

#endif // Q3DSURFACE_P_H
