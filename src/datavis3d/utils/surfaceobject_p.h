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

#ifndef SURFACEOBJECT_P_H
#define SURFACEOBJECT_P_H

#include "datavis3dglobal_p.h"
#include "abstractobjecthelper_p.h"
#include <QOpenGLFunctions>

QT_DATAVIS3D_BEGIN_NAMESPACE

class SurfaceObject : public AbstractObjectHelper
{
public:
    SurfaceObject();
    ~SurfaceObject();

    void setUpData(QList<qreal> series, int columns, int rows, GLfloat yRange, bool changeGeometry);
    void setUpSmoothData(QList<qreal> series, int columns, int rows, GLfloat yRange, bool changeGeometry);
    GLuint gridElementBuf();
    GLuint gridIndexCount();

private:
    QVector3D normal(const QVector3D &a, const QVector3D &b, const QVector3D &c);
    void createBuffers(const QVector<QVector3D> &vertices, const QVector<QVector2D> &uvs,
                       const QVector<QVector3D> &normals, const GLint *indices,
                       const GLint *gridIndices, bool changeGeometry);

private:
    QList<qreal> m_series;
    int m_dataWidth;
    int m_dataDepth;
    GLfloat m_yRange;
    GLuint m_gridElementbuffer;
    GLuint m_gridIndexCount;
};

QT_DATAVIS3D_END_NAMESPACE
#endif // SURFACEOBJECT_P_H
