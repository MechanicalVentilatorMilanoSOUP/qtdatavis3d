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

#ifndef ABSTRACTRENDERITEM_P_H
#define ABSTRACTRENDERITEM_P_H

#include "datavis3dglobal_p.h"
#include "labelitem_p.h"

#include <QOpenGLFunctions>
#include <QString>
#include <QVector3D>

QT_DATAVIS3D_BEGIN_NAMESPACE

class AbstractRenderItem
{
public:
    AbstractRenderItem();
    virtual ~AbstractRenderItem();

    // Position in 3D scene
    inline void setTranslation(const QVector3D &translation) { m_translation = translation; }
    inline const QVector3D &translation() const {return m_translation; }

    // Label item for formatted label
    LabelItem &labelItem();

    // Selection label item (containing special selection texture, if mode is activated)
    LabelItem &selectionLabel();

    // Formatted label for item.
    void setLabel(const QString &label);
    QString &label(); // Formats label if not previously formatted

protected:
    virtual void formatLabel() = 0;

    QString m_label;
    QVector3D m_translation;
    LabelItem *m_labelItem;
    LabelItem *m_selectionLabel;

    friend class QAbstractDataItem;
};

QT_DATAVIS3D_END_NAMESPACE

#endif
