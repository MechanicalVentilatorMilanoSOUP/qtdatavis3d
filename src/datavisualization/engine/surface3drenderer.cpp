/****************************************************************************
**
** Copyright (C) 2014 Digia Plc
** All rights reserved.
** For any questions to Digia, please use contact form at http://qt.digia.com
**
** This file is part of the QtDataVisualization module.
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

#include "surface3drenderer_p.h"
#include "q3dcamera_p.h"
#include "shaderhelper_p.h"
#include "texturehelper_p.h"
#include "utils_p.h"

#include <QtCore/qmath.h>

static const int ID_TO_RGBA_MASK = 0xff;

QT_BEGIN_NAMESPACE_DATAVISUALIZATION

//#define SHOW_DEPTH_TEXTURE_SCENE

// Margin for background (1.10 make it 10% larger to avoid
// selection ball being drawn inside background)
const GLfloat backgroundMargin = 1.1f;
const GLfloat sliceZScale = 0.1f;
const GLfloat sliceUnits = 2.5f;
const uint greenMultiplier = 256;
const uint blueMultiplier = 65536;
const uint alphaMultiplier = 16777216;

Surface3DRenderer::Surface3DRenderer(Surface3DController *controller)
    : Abstract3DRenderer(controller),
      m_cachedIsSlicingActivated(false),
      m_depthShader(0),
      m_backgroundShader(0),
      m_surfaceFlatShader(0),
      m_surfaceSmoothShader(0),
      m_surfaceGridShader(0),
      m_surfaceSliceFlatShader(0),
      m_surfaceSliceSmoothShader(0),
      m_selectionShader(0),
      m_labelShader(0),
      m_heightNormalizer(0.0f),
      m_scaleX(0.0f),
      m_scaleZ(0.0f),
      m_scaleXWithBackground(0.0f),
      m_scaleZWithBackground(0.0f),
      m_depthModelTexture(0),
      m_depthFrameBuffer(0),
      m_selectionFrameBuffer(0),
      m_selectionDepthBuffer(0),
      m_selectionResultTexture(0),
      m_shadowQualityToShader(33.3f),
      m_flatSupported(true),
      m_selectionActive(false),
      m_shadowQualityMultiplier(3),
      m_hasHeightAdjustmentChanged(true),
      m_selectedPoint(Surface3DController::invalidSelectionPosition()),
      m_selectedSeries(0),
      m_clickedPosition(Surface3DController::invalidSelectionPosition()),
      m_selectionTexturesDirty(false),
      m_noShadowTexture(0)
{
    m_axisCacheY.setScale(2.0f);
    m_axisCacheY.setTranslate(-1.0f);

    // Check if flat feature is supported
    ShaderHelper tester(this, QStringLiteral(":/shaders/vertexSurfaceFlat"),
                        QStringLiteral(":/shaders/fragmentSurfaceFlat"));
    if (!tester.testCompile()) {
        m_flatSupported = false;
        connect(this, &Surface3DRenderer::flatShadingSupportedChanged,
                controller, &Surface3DController::handleFlatShadingSupportedChange);
        emit flatShadingSupportedChanged(m_flatSupported);
        qWarning() << "Warning: Flat qualifier not supported on your platform's GLSL language."
                      " Requires at least GLSL version 1.2 with GL_EXT_gpu_shader4 extension.";
    }

    initializeOpenGLFunctions();
    initializeOpenGL();
}

Surface3DRenderer::~Surface3DRenderer()
{
    if (QOpenGLContext::currentContext()) {
        m_textureHelper->glDeleteFramebuffers(1, &m_depthFrameBuffer);
        m_textureHelper->glDeleteRenderbuffers(1, &m_selectionDepthBuffer);
        m_textureHelper->glDeleteFramebuffers(1, &m_selectionFrameBuffer);

        m_textureHelper->deleteTexture(&m_noShadowTexture);
        m_textureHelper->deleteTexture(&m_depthTexture);
        m_textureHelper->deleteTexture(&m_depthModelTexture);
        m_textureHelper->deleteTexture(&m_selectionResultTexture);
    }
    delete m_depthShader;
    delete m_backgroundShader;
    delete m_selectionShader;
    delete m_surfaceFlatShader;
    delete m_surfaceSmoothShader;
    delete m_surfaceGridShader;
    delete m_surfaceSliceFlatShader;
    delete m_surfaceSliceSmoothShader;
    delete m_labelShader;
}

void Surface3DRenderer::initializeOpenGL()
{
    Abstract3DRenderer::initializeOpenGL();

    // Initialize shaders
    initSurfaceShaders();
    initLabelShaders(QStringLiteral(":/shaders/vertexLabel"),
                     QStringLiteral(":/shaders/fragmentLabel"));

#if !defined(QT_OPENGL_ES_2)
    // Init depth shader (for shadows). Init in any case, easier to handle shadow activation if done via api.
    initDepthShader();
#endif

    // Init selection shader
    initSelectionShaders();

#if !(defined QT_OPENGL_ES_2)
    // Load grid line mesh
    loadGridLineMesh();
#endif

    // Load label mesh
    loadLabelMesh();

    // Resize in case we've missed resize events
    // Resize calls initSelectionBuffer and initDepthBuffer, so they don't need to be called here
    handleResize();

    // Load background mesh (we need to be initialized first)
    loadBackgroundMesh();

    // Create texture for no shadows
    QImage image(2, 2, QImage::Format_RGB32);
    image.fill(Qt::white);
    m_noShadowTexture = m_textureHelper->create2DTexture(image, false, true, false, true);
}

void Surface3DRenderer::updateData()
{
    calculateSceneScalingFactors();

    foreach (SeriesRenderCache *baseCache, m_renderCacheList) {
        SurfaceSeriesRenderCache *cache = static_cast<SurfaceSeriesRenderCache *>(baseCache);
        if (cache->isVisible() && cache->dataDirty()) {
            const QSurface3DSeries *currentSeries = cache->series();
            QSurfaceDataProxy *dataProxy = currentSeries->dataProxy();
            const QSurfaceDataArray &array = *dataProxy->array();
            QSurfaceDataArray &dataArray = cache->dataArray();
            QRect sampleSpace;

            // Need minimum of 2x2 array to draw a surface
            if (array.size() >= 2 && array.at(0)->size() >= 2)
                sampleSpace = calculateSampleRect(array);

            bool dimensionsChanged = false;
            if (cache->sampleSpace() != sampleSpace) {
                if (sampleSpace.width() >= 2)
                    m_selectionTexturesDirty = true;

                dimensionsChanged = true;
                cache->setSampleSpace(sampleSpace);

                for (int i = 0; i < dataArray.size(); i++)
                    delete dataArray.at(i);
                dataArray.clear();
            }

            if (sampleSpace.width() >= 2 && sampleSpace.height() >= 2) {
                if (dimensionsChanged) {
                    dataArray.reserve(sampleSpace.height());
                    for (int i = 0; i < sampleSpace.height(); i++)
                        dataArray << new QSurfaceDataRow(sampleSpace.width());
                }
                for (int i = 0; i < sampleSpace.height(); i++) {
                    for (int j = 0; j < sampleSpace.width(); j++) {
                        (*(dataArray.at(i)))[j] = array.at(i + sampleSpace.y())->at(
                                    j + sampleSpace.x());
                    }
                }

                checkFlatSupport(cache);
                updateObjects(cache, dimensionsChanged);
                cache->setFlatStatusDirty(false);
            } else {
                cache->surfaceObject()->clear();
            }
            cache->setDataDirty(false);
        }
    }

    if (m_selectionTexturesDirty && m_cachedSelectionMode > QAbstract3DGraph::SelectionNone)
        updateSelectionTextures();

    updateSelectedPoint(m_selectedPoint, m_selectedSeries);
}

void Surface3DRenderer::updateSeries(const QList<QAbstract3DSeries *> &seriesList)
{
    Abstract3DRenderer::updateSeries(seriesList);

    bool noSelection = true;
    foreach (QAbstract3DSeries *series, seriesList) {
        QSurface3DSeries *surfaceSeries = static_cast<QSurface3DSeries *>(series);
        SurfaceSeriesRenderCache *cache =
                static_cast<SurfaceSeriesRenderCache *>( m_renderCacheList.value(series));
        if (noSelection
                && surfaceSeries->selectedPoint() != QSurface3DSeries::invalidSelectionPosition()) {
            if (selectionLabel() != cache->itemLabel())
                m_selectionLabelDirty = true;
            noSelection = false;
        }

        if (cache->isFlatStatusDirty() && cache->sampleSpace().width()) {
            checkFlatSupport(cache);
            updateObjects(cache, true);
            cache->setFlatStatusDirty(false);
        }
    }

    if (noSelection && !selectionLabel().isEmpty()) {
        m_selectionLabelDirty = true;
        updateSelectedPoint(Surface3DController::invalidSelectionPosition(), 0);
    }

    // Selection pointer issues
    if (m_selectedSeries) {
        foreach (SeriesRenderCache *baseCache, m_renderCacheList) {
            SurfaceSeriesRenderCache *cache = static_cast<SurfaceSeriesRenderCache *>(baseCache);
            QVector4D highlightColor =
                    Utils::vectorFromColor(cache->series()->singleHighlightColor());
            SelectionPointer *slicePointer = cache->sliceSelectionPointer();
            if (slicePointer) {
                slicePointer->setHighlightColor(highlightColor);
                slicePointer->setPointerObject(cache->object());
                slicePointer->setRotation(cache->meshRotation());
            }
            SelectionPointer *mainPointer = cache->mainSelectionPointer();
            if (mainPointer) {
                mainPointer->setHighlightColor(highlightColor);
                mainPointer->setPointerObject(cache->object());
                mainPointer->setRotation(cache->meshRotation());
            }
        }
    }
}

SeriesRenderCache *Surface3DRenderer::createNewCache(QAbstract3DSeries *series)
{
    m_selectionTexturesDirty = true;
    return new SurfaceSeriesRenderCache(series, this);
}

void Surface3DRenderer::cleanCache(SeriesRenderCache *cache)
{
    Abstract3DRenderer::cleanCache(cache);
    m_selectionTexturesDirty = true;
}

void Surface3DRenderer::updateRows(const QVector<Surface3DController::ChangeRow> &rows)
{
    foreach (Surface3DController::ChangeRow item, rows) {
        SurfaceSeriesRenderCache *cache =
                static_cast<SurfaceSeriesRenderCache *>(m_renderCacheList.value(item.series));
        QSurfaceDataArray &dstArray = cache->dataArray();
        const QRect &sampleSpace = cache->sampleSpace();

        const QSurfaceDataArray *srcArray = 0;
        QSurfaceDataProxy *dataProxy = item.series->dataProxy();
        if (dataProxy)
            srcArray = dataProxy->array();

        if (cache && srcArray->size() >= 2 && srcArray->at(0)->size() >= 2 &&
                sampleSpace.width() >= 2 && sampleSpace.height() >= 2) {
            bool updateBuffers = false;
            int sampleSpaceTop = sampleSpace.y() + sampleSpace.height();
            int row = item.row;
            if (row >= sampleSpace.y() && row <= sampleSpaceTop) {
                updateBuffers = true;
                for (int j = 0; j < sampleSpace.width(); j++) {
                    (*(dstArray.at(row - sampleSpace.y())))[j] =
                            srcArray->at(row)->at(j + sampleSpace.x());
                }

                if (cache->isFlatShadingEnabled()) {
                    cache->surfaceObject()->updateCoarseRow(dstArray, row - sampleSpace.y(),
                                                            m_polarGraph);
                } else {
                    cache->surfaceObject()->updateSmoothRow(dstArray, row - sampleSpace.y(),
                                                            m_polarGraph);
                }
            }
            if (updateBuffers)
                cache->surfaceObject()->uploadBuffers();
        }
    }

    updateSelectedPoint(m_selectedPoint, m_selectedSeries);
}

void Surface3DRenderer::updateItems(const QVector<Surface3DController::ChangeItem> &points)
{
    foreach (Surface3DController::ChangeItem item, points) {
        SurfaceSeriesRenderCache *cache =
                static_cast<SurfaceSeriesRenderCache *>(m_renderCacheList.value(item.series));
        QSurfaceDataArray &dstArray = cache->dataArray();
        const QRect &sampleSpace = cache->sampleSpace();

        const QSurfaceDataArray *srcArray = 0;
        QSurfaceDataProxy *dataProxy = item.series->dataProxy();
        if (dataProxy)
            srcArray = dataProxy->array();

        if (cache && srcArray->size() >= 2 && srcArray->at(0)->size() >= 2 &&
                sampleSpace.width() >= 2 && sampleSpace.height() >= 2) {
            int sampleSpaceTop = sampleSpace.y() + sampleSpace.height();
            int sampleSpaceRight = sampleSpace.x() + sampleSpace.width();
            bool updateBuffers = false;
            // Note: Point is (row, column), samplespace is (columns x rows)
            QPoint point = item.point;

            if (point.x() <= sampleSpaceTop && point.x() >= sampleSpace.y() &&
                    point.y() <= sampleSpaceRight && point.y() >= sampleSpace.x()) {
                updateBuffers = true;
                int x = point.y() - sampleSpace.x();
                int y = point.x() - sampleSpace.y();
                (*(dstArray.at(y)))[x] = srcArray->at(point.x())->at(point.y());

                if (cache->isFlatShadingEnabled())
                    cache->surfaceObject()->updateCoarseItem(dstArray, y, x, m_polarGraph);
                else
                    cache->surfaceObject()->updateSmoothItem(dstArray, y, x, m_polarGraph);
            }
            if (updateBuffers)
                cache->surfaceObject()->uploadBuffers();
        }

    }

    updateSelectedPoint(m_selectedPoint, m_selectedSeries);
}

void Surface3DRenderer::updateSliceDataModel(const QPoint &point)
{
    foreach (SeriesRenderCache *baseCache, m_renderCacheList)
        static_cast<SurfaceSeriesRenderCache *>(baseCache)->sliceSurfaceObject()->clear();

    if (m_cachedSelectionMode.testFlag(QAbstract3DGraph::SelectionMultiSeries)) {
        // Find axis coordinates for the selected point
        SeriesRenderCache *selectedCache =
                m_renderCacheList.value(const_cast<QSurface3DSeries *>(m_selectedSeries));
        QSurfaceDataArray &dataArray =
                static_cast<SurfaceSeriesRenderCache *>(selectedCache)->dataArray();
        QSurfaceDataItem item = dataArray.at(point.x())->at(point.y());
        QPointF coords(item.x(), item.z());

        foreach (SeriesRenderCache *baseCache, m_renderCacheList) {
            SurfaceSeriesRenderCache *cache = static_cast<SurfaceSeriesRenderCache *>(baseCache);
            if (cache->series() != m_selectedSeries) {
                QPoint mappedPoint = mapCoordsToSampleSpace(cache, coords);
                updateSliceObject(cache, mappedPoint);
            } else {
                updateSliceObject(cache, point);
            }
        }
    } else {
        if (m_selectedSeries) {
            SurfaceSeriesRenderCache *cache =
                    static_cast<SurfaceSeriesRenderCache *>(
                        m_renderCacheList.value(m_selectedSeries));
            if (cache)
                updateSliceObject(static_cast<SurfaceSeriesRenderCache *>(cache), point);
        }
    }
}

QPoint Surface3DRenderer::mapCoordsToSampleSpace(SurfaceSeriesRenderCache *cache,
                                                 const QPointF &coords)
{
    QPoint point(-1, -1);

    QSurfaceDataArray &dataArray = cache->dataArray();
    int top = dataArray.size() - 1;
    int right = dataArray.at(top)->size() - 1;
    QSurfaceDataItem itemBottomLeft = dataArray.at(0)->at(0);
    QSurfaceDataItem itemTopRight = dataArray.at(top)->at(right);

    if (itemBottomLeft.x() <= coords.x() && itemTopRight.x() >= coords.x()) {
        float modelX = coords.x() - itemBottomLeft.x();
        float spanX = itemTopRight.x() - itemBottomLeft.x();
        float stepX = spanX / float(right);
        int sampleX = int((modelX + (stepX / 2.0f)) / stepX);

        QSurfaceDataItem item = dataArray.at(0)->at(sampleX);
        if (!::qFuzzyCompare(float(coords.x()), item.x())) {
            int direction = 1;
            if (item.x() > coords.x())
                direction = -1;

            findMatchingColumn(coords.x(), sampleX, direction, dataArray);
        }

        if (sampleX >= 0 && sampleX <= right)
            point.setY(sampleX);
    }

    if (itemBottomLeft.z() <= coords.y() && itemTopRight.z() >= coords.y()) {
        float modelY = coords.y() - itemBottomLeft.z();
        float spanY = itemTopRight.z() - itemBottomLeft.z();
        float stepY = spanY / float(top);
        int sampleY = int((modelY + (stepY / 2.0f)) / stepY);

        QSurfaceDataItem item = dataArray.at(sampleY)->at(0);
        if (!::qFuzzyCompare(float(coords.y()), item.z())) {
            int direction = 1;
            if (item.z() > coords.y())
                direction = -1;

            findMatchingRow(coords.y(), sampleY, direction, dataArray);
        }

        if (sampleY >= 0 && sampleY <= top)
            point.setX(sampleY);
    }

    return point;
}

void Surface3DRenderer::findMatchingRow(float z, int &sample, int direction,
                                        QSurfaceDataArray &dataArray)
{
    int maxZ = dataArray.size() - 1;
    QSurfaceDataItem item = dataArray.at(sample)->at(0);
    float distance = qAbs(z - item.z());
    int newSample = sample + direction;
    while (newSample >= 0 && newSample <= maxZ) {
        item = dataArray.at(newSample)->at(0);
        float newDist = qAbs(z - item.z());
        if (newDist < distance) {
            sample = newSample;
            distance = newDist;
        } else {
            break;
        }
        newSample = sample + direction;
    }
}

void Surface3DRenderer::findMatchingColumn(float x, int &sample, int direction,
                                           QSurfaceDataArray &dataArray)
{
    int maxX = dataArray.at(0)->size() - 1;
    QSurfaceDataItem item = dataArray.at(0)->at(sample);
    float distance = qAbs(x - item.x());
    int newSample = sample + direction;
    while (newSample >= 0 && newSample <= maxX) {
        item = dataArray.at(0)->at(newSample);
        float newDist = qAbs(x - item.x());
        if (newDist < distance) {
            sample = newSample;
            distance = newDist;
        } else {
            break;
        }
        newSample = sample + direction;
    }
}

void Surface3DRenderer::updateSliceObject(SurfaceSeriesRenderCache *cache, const QPoint &point)
{
    int column = point.y();
    int row = point.x();

    if ((m_cachedSelectionMode.testFlag(QAbstract3DGraph::SelectionRow) && row == -1) ||
            (m_cachedSelectionMode.testFlag(QAbstract3DGraph::SelectionColumn) && column == -1)) {
        cache->sliceSurfaceObject()->clear();
        return;
    }

    QSurfaceDataArray &sliceDataArray = cache->sliceDataArray();
    for (int i = 0; i < sliceDataArray.size(); i++)
        delete sliceDataArray.at(i);
    sliceDataArray.clear();
    sliceDataArray.reserve(2);

    QSurfaceDataRow *sliceRow;
    QSurfaceDataArray &dataArray = cache->dataArray();
    float adjust = (0.025f * m_heightNormalizer) / 2.0f;
    float doubleAdjust = 2.0f * adjust;
    bool flipZX = false;
    float zBack;
    float zFront;
    if (m_cachedSelectionMode.testFlag(QAbstract3DGraph::SelectionRow)) {
        QSurfaceDataRow *src = dataArray.at(row);
        sliceRow = new QSurfaceDataRow(src->size());
        zBack = m_axisCacheZ.min();
        zFront = m_axisCacheZ.max();
        for (int i = 0; i < sliceRow->size(); i++)
            (*sliceRow)[i].setPosition(QVector3D(src->at(i).x(), src->at(i).y() + adjust, zFront));
    } else {
        flipZX = true;
        const QRect &sampleSpace = cache->sampleSpace();
        sliceRow = new QSurfaceDataRow(sampleSpace.height());
        zBack = m_axisCacheX.min();
        zFront = m_axisCacheX.max();
        for (int i = 0; i < sampleSpace.height(); i++) {
            (*sliceRow)[i].setPosition(QVector3D(dataArray.at(i)->at(column).z(),
                                                 dataArray.at(i)->at(column).y() + adjust,
                                                 zFront));
        }
    }
    sliceDataArray << sliceRow;

    // Make a duplicate, so that we get a little bit depth
    QSurfaceDataRow *duplicateRow = new QSurfaceDataRow(*sliceRow);
    for (int i = 0; i < sliceRow->size(); i++) {
        (*sliceRow)[i].setPosition(QVector3D(sliceRow->at(i).x(),
                                             sliceRow->at(i).y() - doubleAdjust,
                                             zBack));
    }
    sliceDataArray << duplicateRow;

    QRect sliceRect(0, 0, sliceRow->size(), 2);
    if (sliceRow->size() > 0) {
        if (cache->isFlatShadingEnabled()) {
            cache->sliceSurfaceObject()->setUpData(sliceDataArray, sliceRect, true, false, flipZX);
        } else {
            cache->sliceSurfaceObject()->setUpSmoothData(sliceDataArray, sliceRect, true, false,
                                                         flipZX);
        }
    }
}

inline static float getDataValue(const QSurfaceDataArray &array, bool searchRow, int index)
{
    if (searchRow)
        return array.at(0)->at(index).x();
    else
        return array.at(index)->at(0).z();
}

inline static int binarySearchArray(const QSurfaceDataArray &array, int maxIdx, float limitValue,
                                    bool searchRow, bool lowBound, bool ascending)
{
    int min = 0;
    int max = maxIdx;
    int mid = 0;
    int retVal;
    while (max >= min) {
        mid = (min + max) / 2;
        float arrayValue = getDataValue(array, searchRow, mid);
        if (arrayValue == limitValue)
            return mid;
        if (ascending) {
            if (arrayValue < limitValue)
                min = mid + 1;
            else
                max = mid - 1;
        } else {
            if (arrayValue > limitValue)
                min = mid + 1;
            else
                max = mid - 1;
        }
    }

    // Exact match not found, return closest depending on bound.
    // The boundary is between last mid and min/max.
    if (lowBound == ascending) {
        if (mid > max)
            retVal = mid;
        else
            retVal = min;
    } else {
        if (mid > max)
            retVal = max;
        else
            retVal = mid;
    }
    if (retVal < 0 || retVal > maxIdx) {
        retVal = -1;
    } else if (lowBound) {
        if (getDataValue(array, searchRow, retVal) < limitValue)
            retVal = -1;
    } else {
        if (getDataValue(array, searchRow, retVal) > limitValue)
            retVal = -1;
    }
    return retVal;
}

QRect Surface3DRenderer::calculateSampleRect(const QSurfaceDataArray &array)
{
    QRect sampleSpace;

    const int maxRow = array.size() - 1;
    const int maxColumn = array.at(0)->size() - 1;

    // We assume data is ordered sequentially in rows for X-value and in columns for Z-value.
    // Determine if data is ascending or descending in each case.
    const bool ascendingX = array.at(0)->at(0).x() < array.at(0)->at(maxColumn).x();
    const bool ascendingZ = array.at(0)->at(0).z() < array.at(maxRow)->at(0).z();

    int idx = binarySearchArray(array, maxColumn, m_axisCacheX.min(), true, true, ascendingX);
    if (idx != -1) {
        if (ascendingX)
            sampleSpace.setLeft(idx);
        else
            sampleSpace.setRight(idx);
    } else {
        sampleSpace.setWidth(-1); // to indicate nothing needs to be shown
        return sampleSpace;
    }

    idx = binarySearchArray(array, maxColumn, m_axisCacheX.max(), true, false, ascendingX);
    if (idx != -1) {
        if (ascendingX)
            sampleSpace.setRight(idx);
        else
            sampleSpace.setLeft(idx);
    } else {
        sampleSpace.setWidth(-1); // to indicate nothing needs to be shown
        return sampleSpace;
    }

    idx = binarySearchArray(array, maxRow, m_axisCacheZ.min(), false, true, ascendingZ);
    if (idx != -1) {
        if (ascendingZ)
            sampleSpace.setTop(idx);
        else
            sampleSpace.setBottom(idx);
    } else {
        sampleSpace.setWidth(-1); // to indicate nothing needs to be shown
        return sampleSpace;
    }

    idx = binarySearchArray(array, maxRow, m_axisCacheZ.max(), false, false, ascendingZ);
    if (idx != -1) {
        if (ascendingZ)
            sampleSpace.setBottom(idx);
        else
            sampleSpace.setTop(idx);
    } else {
        sampleSpace.setWidth(-1); // to indicate nothing needs to be shown
        return sampleSpace;
    }

    return sampleSpace;
}

void Surface3DRenderer::updateScene(Q3DScene *scene)
{
    // Set initial camera position
    // X must be 0 for rotation to work - we can use "setCameraRotation" for setting it later
    if (m_hasHeightAdjustmentChanged) {
        scene->activeCamera()->d_ptr->setBaseOrientation(cameraDistanceVector, zeroVector,
                                                         upVector);
        // For now this is used just to make things once. Proper use will come
        m_hasHeightAdjustmentChanged = false;
    }

    Abstract3DRenderer::updateScene(scene);

    if (m_selectionActive
            && m_cachedSelectionMode.testFlag(QAbstract3DGraph::SelectionItem)) {
        m_selectionDirty = true; // Ball may need repositioning if scene changes
    }

    updateSlicingActive(scene->isSlicingActive());
}

void Surface3DRenderer::render(GLuint defaultFboHandle)
{
    // Handle GL state setup for FBO buffers and clearing of the render surface
    Abstract3DRenderer::render(defaultFboHandle);

    if (m_axisCacheX.positionsDirty())
        m_axisCacheX.updateAllPositions();
    if (m_axisCacheY.positionsDirty())
        m_axisCacheY.updateAllPositions();
    if (m_axisCacheZ.positionsDirty())
        m_axisCacheZ.updateAllPositions();

    drawScene(defaultFboHandle);
    if (m_cachedIsSlicingActivated)
        drawSlicedScene();

    // Render selection ball
    if (m_selectionActive
            && m_cachedSelectionMode.testFlag(QAbstract3DGraph::SelectionItem)) {
        foreach (SeriesRenderCache *baseCache, m_renderCacheList) {
            SurfaceSeriesRenderCache *cache = static_cast<SurfaceSeriesRenderCache *>(baseCache);
            if (cache->slicePointerActive() && cache->renderable() &&
                    m_cachedIsSlicingActivated ) {
                cache->sliceSelectionPointer()->render(defaultFboHandle);
            }
            if (cache->mainPointerActive() && cache->renderable())
                cache->mainSelectionPointer()->render(defaultFboHandle, m_useOrthoProjection);
        }
    }
}

void Surface3DRenderer::drawSlicedScene()
{
    QVector3D lightPos;

    QVector4D lightColor = Utils::vectorFromColor(m_cachedTheme->lightColor());

    // Specify viewport
    glViewport(m_secondarySubViewport.x(),
               m_secondarySubViewport.y(),
               m_secondarySubViewport.width(),
               m_secondarySubViewport.height());

    // Set up projection matrix
    QMatrix4x4 projectionMatrix;

    GLfloat aspect = (GLfloat)m_secondarySubViewport.width()
            / (GLfloat)m_secondarySubViewport.height();
    GLfloat sliceUnitsScaled = sliceUnits / m_autoScaleAdjustment;
    projectionMatrix.ortho(-sliceUnitsScaled * aspect, sliceUnitsScaled * aspect,
                           -sliceUnitsScaled, sliceUnitsScaled,
                           -1.0f, 4.0f);

    // Set view matrix
    QMatrix4x4 viewMatrix;
    viewMatrix.lookAt(QVector3D(0.0f, 0.0f, 1.0f), zeroVector, upVector);

    // Set light position
    lightPos = QVector3D(0.0f, 0.0f, 2.0f);

    QMatrix4x4 projectionViewMatrix = projectionMatrix * viewMatrix;

    const Q3DCamera *activeCamera = m_cachedScene->activeCamera();

    bool rowMode = m_cachedSelectionMode.testFlag(QAbstract3DGraph::SelectionRow);
    AxisRenderCache &sliceCache = rowMode ? m_axisCacheX : m_axisCacheZ;

    GLfloat scaleXBackground = 0.0f;

    // Disable culling to avoid ugly conditionals with reversed axes and data
    glDisable(GL_CULL_FACE);

    if (!m_renderCacheList.isEmpty()) {
        bool drawGrid = false;

        foreach (SeriesRenderCache *baseCache, m_renderCacheList) {
            SurfaceSeriesRenderCache *cache = static_cast<SurfaceSeriesRenderCache *>(baseCache);
            if (cache->sliceSurfaceObject()->indexCount() && cache->renderable()) {
                if (!drawGrid && cache->surfaceGridVisible()) {
                    glEnable(GL_POLYGON_OFFSET_FILL);
                    glPolygonOffset(0.5f, 1.0f);
                    drawGrid = true;
                }

                if (rowMode)
                    scaleXBackground = m_scaleXWithBackground;
                else
                    scaleXBackground = m_scaleZWithBackground;

                QMatrix4x4 MVPMatrix;
                QMatrix4x4 modelMatrix;
                QMatrix4x4 itModelMatrix;

                QVector3D scaling(1.0f, 1.0f, sliceZScale);
                modelMatrix.scale(scaling);
                itModelMatrix.scale(scaling);

                MVPMatrix = projectionViewMatrix * modelMatrix;
                cache->setMVPMatrix(MVPMatrix);

                if (cache->surfaceVisible()) {
                    ShaderHelper *surfaceShader = m_surfaceSliceSmoothShader;
                    if (cache->isFlatShadingEnabled())
                        surfaceShader = m_surfaceSliceFlatShader;

                    surfaceShader->bind();

                    GLuint colorTexture = cache->baseUniformTexture();;
                    if (cache->colorStyle() != Q3DTheme::ColorStyleUniform)
                        colorTexture = cache->baseGradientTexture();

                    // Set shader bindings
                    surfaceShader->setUniformValue(surfaceShader->lightP(), lightPos);
                    surfaceShader->setUniformValue(surfaceShader->view(), viewMatrix);
                    surfaceShader->setUniformValue(surfaceShader->model(), modelMatrix);
                    surfaceShader->setUniformValue(surfaceShader->nModel(),
                                                   itModelMatrix.inverted().transposed());
                    surfaceShader->setUniformValue(surfaceShader->MVP(), MVPMatrix);
                    surfaceShader->setUniformValue(surfaceShader->lightS(), 0.15f);
                    surfaceShader->setUniformValue(surfaceShader->ambientS(),
                                                   m_cachedTheme->ambientLightStrength() * 2.3f);
                    surfaceShader->setUniformValue(surfaceShader->lightColor(), lightColor);

                    m_drawer->drawObject(surfaceShader, cache->sliceSurfaceObject(), colorTexture);
                }
            }
        }

        // Draw surface grid
        if (drawGrid) {
            glDisable(GL_POLYGON_OFFSET_FILL);
            m_surfaceGridShader->bind();
            m_surfaceGridShader->setUniformValue(m_surfaceGridShader->color(),
                                                 Utils::vectorFromColor(m_cachedTheme->gridLineColor()));
            foreach (SeriesRenderCache *baseCache, m_renderCacheList) {
                SurfaceSeriesRenderCache *cache =
                        static_cast<SurfaceSeriesRenderCache *>(baseCache);
                if (cache->sliceSurfaceObject()->indexCount() && cache->isVisible() &&
                        cache->surfaceGridVisible()) {
                    m_surfaceGridShader->setUniformValue(m_surfaceGridShader->MVP(),
                                                         cache->MVPMatrix());
                    m_drawer->drawSurfaceGrid(m_surfaceGridShader, cache->sliceSurfaceObject());
                }
            }
        }
    }

    // Disable textures
    glDisable(GL_TEXTURE_2D);

    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);

    // Grid lines
    if (m_cachedTheme->isGridEnabled() && m_heightNormalizer) {
#if !(defined QT_OPENGL_ES_2)
        ShaderHelper *lineShader = m_backgroundShader;
#else
        ShaderHelper *lineShader = m_selectionShader; // Plain color shader for GL_LINES
#endif

        // Bind line shader
        lineShader->bind();

        // Set unchanging shader bindings
        QVector4D lineColor = Utils::vectorFromColor(m_cachedTheme->gridLineColor());
        lineShader->setUniformValue(lineShader->lightP(), lightPos);
        lineShader->setUniformValue(lineShader->view(), viewMatrix);
        lineShader->setUniformValue(lineShader->color(), lineColor);
        lineShader->setUniformValue(lineShader->ambientS(),
                                    m_cachedTheme->ambientLightStrength() * 2.3f);
        lineShader->setUniformValue(lineShader->lightS(), 0.0f);
        lineShader->setUniformValue(lineShader->lightColor(), lightColor);

        // Horizontal lines
        int gridLineCount = m_axisCacheY.gridLineCount();
        if (m_axisCacheY.segmentCount() > 0) {
            QVector3D gridLineScaleX(scaleXBackground, gridLineWidth, gridLineWidth);

            for (int line = 0; line < gridLineCount; line++) {
                QMatrix4x4 modelMatrix;
                QMatrix4x4 MVPMatrix;
                QMatrix4x4 itModelMatrix;

                modelMatrix.translate(0.0f, m_axisCacheY.gridLinePosition(line), -1.0f);

                modelMatrix.scale(gridLineScaleX);
                itModelMatrix.scale(gridLineScaleX);

                MVPMatrix = projectionViewMatrix * modelMatrix;

                // Set the rest of the shader bindings
                lineShader->setUniformValue(lineShader->model(), modelMatrix);
                lineShader->setUniformValue(lineShader->nModel(),
                                            itModelMatrix.inverted().transposed());
                lineShader->setUniformValue(lineShader->MVP(), MVPMatrix);

                // Draw the object
#if !(defined QT_OPENGL_ES_2)
                m_drawer->drawObject(lineShader, m_gridLineObj);
#else
                m_drawer->drawLine(lineShader);
#endif
            }
        }

        // Vertical lines
        QVector3D gridLineScaleY(gridLineWidth, backgroundMargin, gridLineWidth);

        gridLineCount = sliceCache.gridLineCount();
        for (int line = 0; line < gridLineCount; line++) {
            QMatrix4x4 modelMatrix;
            QMatrix4x4 MVPMatrix;
            QMatrix4x4 itModelMatrix;

            modelMatrix.translate(sliceCache.gridLinePosition(line), 0.0f, -1.0f);
            modelMatrix.scale(gridLineScaleY);
            itModelMatrix.scale(gridLineScaleY);
#if (defined QT_OPENGL_ES_2)
            modelMatrix.rotate(m_zRightAngleRotation);
            itModelMatrix.rotate(m_zRightAngleRotation);
#endif

            MVPMatrix = projectionViewMatrix * modelMatrix;

            // Set the rest of the shader bindings
            lineShader->setUniformValue(lineShader->model(), modelMatrix);
            lineShader->setUniformValue(lineShader->nModel(),
                                        itModelMatrix.inverted().transposed());
            lineShader->setUniformValue(lineShader->MVP(), MVPMatrix);

            // Draw the object
#if !(defined QT_OPENGL_ES_2)
            m_drawer->drawObject(lineShader, m_gridLineObj);
#else
            m_drawer->drawLine(lineShader);
#endif
        }
    }

    // Draw labels
    m_labelShader->bind();
    glEnable(GL_TEXTURE_2D);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // Y Labels to back wall
    int labelNbr = 0;

    QVector3D positionComp(0.0f, 0.0f, 0.0f);
    QVector3D labelTrans = QVector3D(scaleXBackground + labelMargin, 0.0f, 0.0f);
    int labelCount = m_axisCacheY.labelCount();
    for (int label = 0; label < labelCount; label++) {
        if (m_axisCacheY.labelItems().size() > labelNbr) {
            labelTrans.setY(m_axisCacheY.labelPosition(label));
            const LabelItem &axisLabelItem = *m_axisCacheY.labelItems().at(labelNbr);

            // Draw the label here
            m_dummyRenderItem.setTranslation(labelTrans);
            m_drawer->drawLabel(m_dummyRenderItem, axisLabelItem, viewMatrix, projectionMatrix,
                                positionComp, identityQuaternion, 0, m_cachedSelectionMode,
                                m_labelShader, m_labelObj, activeCamera, true, true,
                                Drawer::LabelMid, Qt::AlignLeft, true);
        }
        labelNbr++;
    }

    // X Labels to ground
    int countLabelItems = sliceCache.labelItems().size();

    QVector3D rotation(0.0f, 0.0f, -45.0f);
    QQuaternion totalRotation = Utils::calculateRotation(rotation);

    labelNbr = 0;
    positionComp.setY(-0.1f);
    labelTrans.setY(-backgroundMargin);
    labelCount = sliceCache.labelCount();
    for (int label = 0; label < labelCount; label++) {
        if (countLabelItems > labelNbr) {
            // Draw the label here
            labelTrans.setX(sliceCache.labelPosition(label));

            m_dummyRenderItem.setTranslation(labelTrans);

            LabelItem *axisLabelItem;
            axisLabelItem = sliceCache.labelItems().at(labelNbr);

            m_drawer->drawLabel(m_dummyRenderItem, *axisLabelItem, viewMatrix, projectionMatrix,
                                positionComp, totalRotation, 0, QAbstract3DGraph::SelectionRow,
                                m_labelShader, m_labelObj, activeCamera,
                                false, false, Drawer::LabelBelow,
                                Qt::AlignmentFlag(Qt::AlignLeft | Qt::AlignTop), true);
        }
        labelNbr++;
    }

    // Draw labels for axes
    AbstractRenderItem *dummyItem(0);
    positionComp.setY(m_autoScaleAdjustment);
    m_drawer->drawLabel(*dummyItem, sliceCache.titleItem(), viewMatrix, projectionMatrix,
                        positionComp, identityQuaternion, 0, m_cachedSelectionMode, m_labelShader,
                        m_labelObj, activeCamera, false, false, Drawer::LabelBottom,
                        Qt::AlignCenter, true);

    // Y-axis label
    rotation = QVector3D(0.0f, 0.0f, 90.0f);
    totalRotation = Utils::calculateRotation(rotation);
    labelTrans = QVector3D(-scaleXBackground - labelMargin, 0.0f, 0.0f);
    m_dummyRenderItem.setTranslation(labelTrans);
    m_drawer->drawLabel(m_dummyRenderItem, m_axisCacheY.titleItem(), viewMatrix,
                        projectionMatrix, zeroVector, totalRotation, 0,
                        m_cachedSelectionMode, m_labelShader, m_labelObj, activeCamera,
                        false, false, Drawer::LabelMid, Qt::AlignBottom);

    glDisable(GL_TEXTURE_2D);
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);

    // Release shader
    glUseProgram(0);
}

void Surface3DRenderer::drawScene(GLuint defaultFboHandle)
{
    bool noShadows = true;

    GLfloat backgroundRotation = 0;
    QVector4D lightColor = Utils::vectorFromColor(m_cachedTheme->lightColor());

    glViewport(m_primarySubViewport.x(),
               m_primarySubViewport.y(),
               m_primarySubViewport.width(),
               m_primarySubViewport.height());

    // Set up projection matrix
    QMatrix4x4 projectionMatrix;
    GLfloat viewPortRatio = (GLfloat)m_primarySubViewport.width()
            / (GLfloat)m_primarySubViewport.height();
    if (m_useOrthoProjection) {
        GLfloat orthoRatio = 2.0f;
        projectionMatrix.ortho(-viewPortRatio * orthoRatio, viewPortRatio * orthoRatio,
                               -orthoRatio, orthoRatio,
                               0.0f, 100.0f);
    } else {
        projectionMatrix.perspective(45.0f, viewPortRatio, 0.1f, 100.0f);
    }

    const Q3DCamera *activeCamera = m_cachedScene->activeCamera();

    // Calculate view matrix
    QMatrix4x4 viewMatrix = activeCamera->d_ptr->viewMatrix();

    QMatrix4x4 projectionViewMatrix = projectionMatrix * viewMatrix;

    // Calculate flipping indicators
    if (viewMatrix.row(0).x() > 0)
        m_zFlipped = false;
    else
        m_zFlipped = true;
    if (viewMatrix.row(0).z() <= 0)
        m_xFlipped = false;
    else
        m_xFlipped = true;

    if (m_flipHorizontalGrid) {
        // Need to determine if camera is below graph top
        float distanceToCenter = activeCamera->position().length()
                / activeCamera->zoomLevel() / m_autoScaleAdjustment * 100.0f;
        qreal cameraAngle = qreal(activeCamera->yRotation()) / 180.0 * M_PI;
        float cameraYPos = float(qSin(cameraAngle)) * distanceToCenter;
        m_yFlippedForGrid = cameraYPos < backgroundMargin;
    } else {
        m_yFlippedForGrid = m_yFlipped;
    }

    // calculate background rotation based on view matrix rotation
    if (viewMatrix.row(0).x() > 0 && viewMatrix.row(0).z() <= 0)
        backgroundRotation = 270.0f;
    else if (viewMatrix.row(0).x() > 0 && viewMatrix.row(0).z() > 0)
        backgroundRotation = 180.0f;
    else if (viewMatrix.row(0).x() <= 0 && viewMatrix.row(0).z() > 0)
        backgroundRotation = 90.0f;
    else if (viewMatrix.row(0).x() <= 0 && viewMatrix.row(0).z() <= 0)
        backgroundRotation = 0.0f;

    QVector3D lightPos = m_cachedScene->activeLight()->position();

    QMatrix4x4 depthViewMatrix;
    QMatrix4x4 depthProjectionMatrix;
    QMatrix4x4 depthProjectionViewMatrix;

    // Draw depth buffer
#if !defined(QT_OPENGL_ES_2)
    GLfloat adjustedLightStrength = m_cachedTheme->lightStrength() / 10.0f;
    if (m_cachedShadowQuality > QAbstract3DGraph::ShadowQualityNone &&
            (!m_renderCacheList.isEmpty() || !m_customRenderCache.isEmpty())) {
        // Render scene into a depth texture for using with shadow mapping
        // Enable drawing to depth framebuffer
        glBindFramebuffer(GL_FRAMEBUFFER, m_depthFrameBuffer);

        // Attach texture to depth attachment
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D,
                               m_depthTexture, 0);
        glClear(GL_DEPTH_BUFFER_BIT);

        // Bind depth shader
        m_depthShader->bind();

        // Set viewport for depth map rendering. Must match texture size. Larger values give smoother shadows.
        glViewport(0, 0,
                   m_primarySubViewport.width() * m_shadowQualityMultiplier,
                   m_primarySubViewport.height() * m_shadowQualityMultiplier);

        // Get the depth view matrix
        // It may be possible to hack lightPos here if we want to make some tweaks to shadow
        QVector3D depthLightPos = activeCamera->d_ptr->calculatePositionRelativeToCamera(
                    zeroVector, 0.0f, 4.0f / m_autoScaleAdjustment);
        depthViewMatrix.lookAt(depthLightPos, zeroVector, upVector);

        // Set the depth projection matrix
        depthProjectionMatrix.perspective(10.0f, (GLfloat)m_primarySubViewport.width()
                                          / (GLfloat)m_primarySubViewport.height(), 3.0f, 100.0f);
        depthProjectionViewMatrix = depthProjectionMatrix * depthViewMatrix;

        glDisable(GL_CULL_FACE);

        foreach (SeriesRenderCache *baseCache, m_renderCacheList) {
            SurfaceSeriesRenderCache *cache = static_cast<SurfaceSeriesRenderCache *>(baseCache);
            SurfaceObject *object = cache->surfaceObject();
            if (object->indexCount() && cache->surfaceVisible() && cache->isVisible()
                    && cache->sampleSpace().width() >= 2 && cache->sampleSpace().height() >= 2) {
                QMatrix4x4 modelMatrix;
                QMatrix4x4 MVPMatrix;

                MVPMatrix = depthProjectionViewMatrix * modelMatrix;
                cache->setMVPMatrix(MVPMatrix);
                m_depthShader->setUniformValue(m_depthShader->MVP(), MVPMatrix);

                // 1st attribute buffer : vertices
                glEnableVertexAttribArray(m_depthShader->posAtt());
                glBindBuffer(GL_ARRAY_BUFFER, object->vertexBuf());
                glVertexAttribPointer(m_depthShader->posAtt(), 3, GL_FLOAT, GL_FALSE, 0,
                                      (void *)0);

                // Index buffer
                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, object->elementBuf());

                // Draw the triangles
                glDrawElements(GL_TRIANGLES, object->indexCount(),
                               object->indicesType(), (void *)0);
            }
        }

        glEnable(GL_CULL_FACE);
        glCullFace(GL_FRONT);

        Abstract3DRenderer::drawCustomItems(RenderingDepth, m_depthShader, viewMatrix,
                                            projectionViewMatrix, depthProjectionViewMatrix,
                                            m_depthTexture, m_shadowQualityToShader);

        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D,
                               m_depthModelTexture, 0);
        glClear(GL_DEPTH_BUFFER_BIT);

        foreach (SeriesRenderCache *baseCache, m_renderCacheList) {
            SurfaceSeriesRenderCache *cache = static_cast<SurfaceSeriesRenderCache *>(baseCache);
            SurfaceObject *object = cache->surfaceObject();
            if (object->indexCount() && cache->surfaceVisible() && cache->isVisible()
                    && cache->sampleSpace().width() >= 2 && cache->sampleSpace().height() >= 2) {
                m_depthShader->setUniformValue(m_depthShader->MVP(), cache->MVPMatrix());

                // 1st attribute buffer : vertices
                glEnableVertexAttribArray(m_depthShader->posAtt());
                glBindBuffer(GL_ARRAY_BUFFER, object->vertexBuf());
                glVertexAttribPointer(m_depthShader->posAtt(), 3, GL_FLOAT, GL_FALSE, 0,
                                      (void *)0);

                // Index buffer
                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, object->elementBuf());

                // Draw the triangles
                glDrawElements(GL_TRIANGLES, object->indexCount(),
                               object->indicesType(), (void *)0);
            }
        }

        // Free buffers
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);

        glDisableVertexAttribArray(m_depthShader->posAtt());

        Abstract3DRenderer::drawCustomItems(RenderingDepth, m_depthShader, viewMatrix,
                                            projectionViewMatrix, depthProjectionViewMatrix,
                                            m_depthTexture, m_shadowQualityToShader);

        // Disable drawing to depth framebuffer (= enable drawing to screen)
        glBindFramebuffer(GL_FRAMEBUFFER, defaultFboHandle);

        // Revert to original viewport
        glViewport(m_primarySubViewport.x(),
                   m_primarySubViewport.y(),
                   m_primarySubViewport.width(),
                   m_primarySubViewport.height());

        // Reset culling to normal
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
    }
#endif
    // Enable texturing
    glEnable(GL_TEXTURE_2D);

    // Draw selection buffer
    if (!m_cachedIsSlicingActivated && (!m_renderCacheList.isEmpty()
                                        || !m_customRenderCache.isEmpty())
            && m_selectionState == SelectOnScene
            && m_cachedSelectionMode > QAbstract3DGraph::SelectionNone
            && m_selectionResultTexture) {
        m_selectionShader->bind();
        glBindFramebuffer(GL_FRAMEBUFFER, m_selectionFrameBuffer);
        glViewport(0,
                   0,
                   m_primarySubViewport.width(),
                   m_primarySubViewport.height());

        glEnable(GL_DEPTH_TEST); // Needed, otherwise the depth render buffer is not used
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT); // Needed for clearing the frame buffer
        glDisable(GL_DITHER); // disable dithering, it may affect colors if enabled

        glDisable(GL_CULL_FACE);

        foreach (SeriesRenderCache *baseCache, m_renderCacheList) {
            SurfaceSeriesRenderCache *cache = static_cast<SurfaceSeriesRenderCache *>(baseCache);
            if (cache->surfaceObject()->indexCount() && cache->renderable()) {
                QMatrix4x4 modelMatrix;
                QMatrix4x4 MVPMatrix;

                MVPMatrix = projectionViewMatrix * modelMatrix;
                m_selectionShader->setUniformValue(m_selectionShader->MVP(), MVPMatrix);

                m_drawer->drawObject(m_selectionShader, cache->surfaceObject(),
                                     cache->selectionTexture());
            }
        }
        m_surfaceGridShader->bind();
        Abstract3DRenderer::drawCustomItems(RenderingSelection, m_surfaceGridShader, viewMatrix,
                                            projectionViewMatrix, depthProjectionViewMatrix,
                                            m_depthTexture, m_shadowQualityToShader);
        drawLabels(true, activeCamera, viewMatrix, projectionMatrix);

        glEnable(GL_DITHER);

        QVector4D clickedColor = Utils::getSelection(m_inputPosition, m_viewport.height());

        glBindFramebuffer(GL_FRAMEBUFFER, defaultFboHandle);

        // Put the RGBA value back to uint
        uint selectionId = uint(clickedColor.x())
                + uint(clickedColor.y()) * greenMultiplier
                + uint(clickedColor.z()) * blueMultiplier
                + uint(clickedColor.w()) * alphaMultiplier;

        m_clickedPosition = selectionIdToSurfacePoint(selectionId);

        emit needRender();

        // Revert to original viewport
        glViewport(m_primarySubViewport.x(),
                   m_primarySubViewport.y(),
                   m_primarySubViewport.width(),
                   m_primarySubViewport.height());
    }

    // Draw the surface
    if (!m_renderCacheList.isEmpty()) {
        // For surface we can see climpses from underneath
        glDisable(GL_CULL_FACE);

        bool drawGrid = false;

        foreach (SeriesRenderCache *baseCache, m_renderCacheList) {
            SurfaceSeriesRenderCache *cache = static_cast<SurfaceSeriesRenderCache *>(baseCache);
            QMatrix4x4 modelMatrix;
            QMatrix4x4 MVPMatrix;
            QMatrix4x4 itModelMatrix;

#ifdef SHOW_DEPTH_TEXTURE_SCENE
            MVPMatrix = depthProjectionViewMatrix * modelMatrix;
#else
            MVPMatrix = projectionViewMatrix * modelMatrix;
#endif
            cache->setMVPMatrix(MVPMatrix);

            const QRect &sampleSpace = cache->sampleSpace();
            if (cache->surfaceObject()->indexCount() && cache->isVisible() &&
                    sampleSpace.width() >= 2 && sampleSpace.height() >= 2) {
                noShadows = false;
                if (!drawGrid && cache->surfaceGridVisible()) {
                    glEnable(GL_POLYGON_OFFSET_FILL);
                    glPolygonOffset(0.5f, 1.0f);
                    drawGrid = true;
                }

                if (cache->surfaceVisible()) {
                    ShaderHelper *shader = m_surfaceFlatShader;
                    if (!cache->isFlatShadingEnabled())
                        shader = m_surfaceSmoothShader;
                    shader->bind();

                    // Set shader bindings
                    shader->setUniformValue(shader->lightP(), lightPos);
                    shader->setUniformValue(shader->view(), viewMatrix);
                    shader->setUniformValue(shader->model(), modelMatrix);
                    shader->setUniformValue(shader->nModel(),
                                            itModelMatrix.inverted().transposed());
                    shader->setUniformValue(shader->MVP(), MVPMatrix);
                    shader->setUniformValue(shader->ambientS(),
                                            m_cachedTheme->ambientLightStrength());
                    shader->setUniformValue(shader->lightColor(), lightColor);

                    GLuint gradientTexture;
                    if (cache->colorStyle() == Q3DTheme::ColorStyleUniform)
                        gradientTexture = cache->baseUniformTexture();
                    else
                        gradientTexture = cache->baseGradientTexture();

#if !defined(QT_OPENGL_ES_2)
                    if (m_cachedShadowQuality > QAbstract3DGraph::ShadowQualityNone) {
                        // Set shadow shader bindings
                        QMatrix4x4 depthMVPMatrix = depthProjectionViewMatrix * modelMatrix;
                        shader->setUniformValue(shader->shadowQ(), m_shadowQualityToShader);
                        shader->setUniformValue(shader->depth(), depthMVPMatrix);
                        shader->setUniformValue(shader->lightS(), adjustedLightStrength);

                        // Draw the objects
                        m_drawer->drawObject(shader, cache->surfaceObject(), gradientTexture,
                                             m_depthModelTexture);
                    } else
#endif
                    {
                        // Set shadowless shader bindings
                        shader->setUniformValue(shader->lightS(),
                                                m_cachedTheme->lightStrength());

                        // Draw the objects
                        m_drawer->drawObject(shader, cache->surfaceObject(), gradientTexture);
                    }
                }
            }
        }
        glEnable(GL_CULL_FACE);

        // Draw surface grid
        if (drawGrid) {
            glDisable(GL_POLYGON_OFFSET_FILL);
            m_surfaceGridShader->bind();
            m_surfaceGridShader->setUniformValue(m_surfaceGridShader->color(),
                                                 Utils::vectorFromColor(
                                                     m_cachedTheme->gridLineColor()));
            foreach (SeriesRenderCache *baseCache, m_renderCacheList) {
                SurfaceSeriesRenderCache *cache =
                        static_cast<SurfaceSeriesRenderCache *>(baseCache);
                m_surfaceGridShader->setUniformValue(m_surfaceGridShader->MVP(),
                                                     cache->MVPMatrix());

                const QRect &sampleSpace = cache->sampleSpace();
                if (cache->surfaceObject()->indexCount() && cache->surfaceGridVisible()
                        && cache->isVisible() && sampleSpace.width() >= 2
                        && sampleSpace.height() >= 2) {
                    m_drawer->drawSurfaceGrid(m_surfaceGridShader, cache->surfaceObject());
                }
            }
        }
    }

    // Bind background shader
    m_backgroundShader->bind();
    glCullFace(GL_BACK);

    // Draw background
    if (m_cachedTheme->isBackgroundEnabled() && m_backgroundObj) {
        QMatrix4x4 modelMatrix;
        QMatrix4x4 MVPMatrix;
        QMatrix4x4 itModelMatrix;

        QVector3D bgScale(m_scaleXWithBackground, backgroundMargin, m_scaleZWithBackground);
        modelMatrix.scale(bgScale);

        // If we're viewing from below, background object must be flipped
        if (m_yFlipped) {
            modelMatrix.rotate(m_xFlipRotation);
            modelMatrix.rotate(270.0f - backgroundRotation, 0.0f, 1.0f, 0.0f);
        } else {
            modelMatrix.rotate(backgroundRotation, 0.0f, 1.0f, 0.0f);
        }

        itModelMatrix = modelMatrix; // Only scaling and rotations, can be used directly

#ifdef SHOW_DEPTH_TEXTURE_SCENE
        MVPMatrix = depthProjectionViewMatrix * modelMatrix;
#else
        MVPMatrix = projectionViewMatrix * modelMatrix;
#endif

        QVector4D backgroundColor = Utils::vectorFromColor(m_cachedTheme->backgroundColor());

        // Set shader bindings
        m_backgroundShader->setUniformValue(m_backgroundShader->lightP(), lightPos);
        m_backgroundShader->setUniformValue(m_backgroundShader->view(), viewMatrix);
        m_backgroundShader->setUniformValue(m_backgroundShader->model(), modelMatrix);
        m_backgroundShader->setUniformValue(m_backgroundShader->nModel(),
                                            itModelMatrix.inverted().transposed());
        m_backgroundShader->setUniformValue(m_backgroundShader->MVP(), MVPMatrix);
        m_backgroundShader->setUniformValue(m_backgroundShader->color(), backgroundColor);
        m_backgroundShader->setUniformValue(m_backgroundShader->ambientS(),
                                            m_cachedTheme->ambientLightStrength() * 2.0f);
        m_backgroundShader->setUniformValue(m_backgroundShader->lightColor(), lightColor);

#if !defined(QT_OPENGL_ES_2)
        if (m_cachedShadowQuality > QAbstract3DGraph::ShadowQualityNone) {
            // Set shadow shader bindings
            QMatrix4x4 depthMVPMatrix = depthProjectionViewMatrix * modelMatrix;
            m_backgroundShader->setUniformValue(m_backgroundShader->shadowQ(),
                                                m_shadowQualityToShader);
            m_backgroundShader->setUniformValue(m_backgroundShader->depth(), depthMVPMatrix);
            m_backgroundShader->setUniformValue(m_backgroundShader->lightS(),
                                                adjustedLightStrength);
            // Draw the object
            if (noShadows && m_customRenderCache.isEmpty())
                m_drawer->drawObject(m_backgroundShader, m_backgroundObj, 0, m_noShadowTexture);
            else
                m_drawer->drawObject(m_backgroundShader, m_backgroundObj, 0, m_depthTexture);
        } else
#else
        Q_UNUSED(noShadows);
#endif
        {
            // Set shadowless shader bindings
            m_backgroundShader->setUniformValue(m_backgroundShader->lightS(),
                                                m_cachedTheme->lightStrength());

            // Draw the object
            m_drawer->drawObject(m_backgroundShader, m_backgroundObj);
        }
    }

    // Draw grid lines
    QVector3D gridLineScaleX(m_scaleXWithBackground, gridLineWidth, gridLineWidth);
    QVector3D gridLineScaleZ(gridLineWidth, gridLineWidth, m_scaleZWithBackground);
    QVector3D gridLineScaleY(gridLineWidth, backgroundMargin, gridLineWidth);

    if (m_cachedTheme->isGridEnabled() && m_heightNormalizer) {
#if !(defined QT_OPENGL_ES_2)
        ShaderHelper *lineShader = m_backgroundShader;
#else
        ShaderHelper *lineShader = m_selectionShader; // Plain color shader for GL_LINES
#endif

        // Bind line shader
        lineShader->bind();

        // Set unchanging shader bindings
        QVector4D lineColor = Utils::vectorFromColor(m_cachedTheme->gridLineColor());
        lineShader->setUniformValue(lineShader->lightP(), lightPos);
        lineShader->setUniformValue(lineShader->view(), viewMatrix);
        lineShader->setUniformValue(lineShader->color(), lineColor);
        lineShader->setUniformValue(lineShader->ambientS(), m_cachedTheme->ambientLightStrength());
        lineShader->setUniformValue(lineShader->lightColor(), lightColor);
#if !defined(QT_OPENGL_ES_2)
        if (m_cachedShadowQuality > QAbstract3DGraph::ShadowQualityNone) {
            // Set shadowed shader bindings
            lineShader->setUniformValue(lineShader->shadowQ(), m_shadowQualityToShader);
            lineShader->setUniformValue(lineShader->lightS(),
                                        m_cachedTheme->lightStrength() / 20.0f);
        } else
#endif
        {
            // Set shadowless shader bindings
            lineShader->setUniformValue(lineShader->lightS(),
                                        m_cachedTheme->lightStrength() / 2.5f);
        }

        QQuaternion lineYRotation;
        QQuaternion lineXRotation;

        if (m_xFlipped)
            lineYRotation = m_yRightAngleRotationNeg;
        else
            lineYRotation = m_yRightAngleRotation;

        if (m_yFlippedForGrid)
            lineXRotation = m_xRightAngleRotation;
        else
            lineXRotation = m_xRightAngleRotationNeg;

        float yFloorLinePosition = -backgroundMargin + gridLineOffset;
        if (m_yFlipped != m_flipHorizontalGrid)
            yFloorLinePosition = -yFloorLinePosition;

        // Rows (= Z)
        if (m_axisCacheZ.segmentCount() > 0) {
            int gridLineCount = m_axisCacheZ.gridLineCount();
            // Floor lines
            if (m_polarGraph) {
                drawRadialGrid(lineShader, yFloorLinePosition, projectionViewMatrix,
                               depthProjectionViewMatrix);
            } else {
                for (int line = 0; line < gridLineCount; line++) {
                    QMatrix4x4 modelMatrix;
                    QMatrix4x4 MVPMatrix;
                    QMatrix4x4 itModelMatrix;

                    modelMatrix.translate(0.0f, yFloorLinePosition,
                                          m_axisCacheZ.gridLinePosition(line));

                    modelMatrix.scale(gridLineScaleX);
                    itModelMatrix.scale(gridLineScaleX);

                    modelMatrix.rotate(lineXRotation);
                    itModelMatrix.rotate(lineXRotation);

                    MVPMatrix = projectionViewMatrix * modelMatrix;

                    // Set the rest of the shader bindings
                    lineShader->setUniformValue(lineShader->model(), modelMatrix);
                    lineShader->setUniformValue(lineShader->nModel(),
                                                itModelMatrix.inverted().transposed());
                    lineShader->setUniformValue(lineShader->MVP(), MVPMatrix);

#if !defined(QT_OPENGL_ES_2)
                    if (m_cachedShadowQuality > QAbstract3DGraph::ShadowQualityNone) {
                        // Set shadow shader bindings
                        QMatrix4x4 depthMVPMatrix = depthProjectionViewMatrix * modelMatrix;
                        lineShader->setUniformValue(lineShader->depth(), depthMVPMatrix);
                        // Draw the object
                        m_drawer->drawObject(lineShader, m_gridLineObj, 0, m_depthTexture);
                    } else {
                        // Draw the object
                        m_drawer->drawObject(lineShader, m_gridLineObj);
                    }
#else
                    m_drawer->drawLine(lineShader);
#endif
                }
                // Side wall lines
                GLfloat lineXTrans = m_scaleXWithBackground - gridLineOffset;

                if (!m_xFlipped)
                    lineXTrans = -lineXTrans;

                for (int line = 0; line < gridLineCount; line++) {
                    QMatrix4x4 modelMatrix;
                    QMatrix4x4 MVPMatrix;
                    QMatrix4x4 itModelMatrix;

                    modelMatrix.translate(lineXTrans, 0.0f, m_axisCacheZ.gridLinePosition(line));

                    modelMatrix.scale(gridLineScaleY);
                    itModelMatrix.scale(gridLineScaleY);

#if !defined(QT_OPENGL_ES_2)
                    modelMatrix.rotate(lineYRotation);
                    itModelMatrix.rotate(lineYRotation);
#else
                    modelMatrix.rotate(m_zRightAngleRotation);
                    itModelMatrix.rotate(m_zRightAngleRotation);
#endif

                    MVPMatrix = projectionViewMatrix * modelMatrix;

                    // Set the rest of the shader bindings
                    lineShader->setUniformValue(lineShader->model(), modelMatrix);
                    lineShader->setUniformValue(lineShader->nModel(),
                                                itModelMatrix.inverted().transposed());
                    lineShader->setUniformValue(lineShader->MVP(), MVPMatrix);

#if !defined(QT_OPENGL_ES_2)
                    if (m_cachedShadowQuality > QAbstract3DGraph::ShadowQualityNone) {
                        // Set shadow shader bindings
                        QMatrix4x4 depthMVPMatrix = depthProjectionViewMatrix * modelMatrix;
                        lineShader->setUniformValue(lineShader->depth(), depthMVPMatrix);
                        // Draw the object
                        m_drawer->drawObject(lineShader, m_gridLineObj, 0, m_depthTexture);
                    } else {
                        // Draw the object
                        m_drawer->drawObject(lineShader, m_gridLineObj);
                    }
#else
                    m_drawer->drawLine(lineShader);
#endif
                }
            }
        }

        // Columns (= X)
        if (m_axisCacheX.segmentCount() > 0) {
#if defined(QT_OPENGL_ES_2)
            lineXRotation = m_yRightAngleRotation;
#endif
            // Floor lines
            int gridLineCount = m_axisCacheX.gridLineCount();

            if (m_polarGraph) {
                drawAngularGrid(lineShader, yFloorLinePosition, projectionViewMatrix,
                               depthProjectionViewMatrix);
            } else {
                for (int line = 0; line < gridLineCount; line++) {
                    QMatrix4x4 modelMatrix;
                    QMatrix4x4 MVPMatrix;
                    QMatrix4x4 itModelMatrix;

                    modelMatrix.translate(m_axisCacheX.gridLinePosition(line), yFloorLinePosition,
                                          0.0f);

                    modelMatrix.scale(gridLineScaleZ);
                    itModelMatrix.scale(gridLineScaleZ);

                    modelMatrix.rotate(lineXRotation);
                    itModelMatrix.rotate(lineXRotation);

                    MVPMatrix = projectionViewMatrix * modelMatrix;

                    // Set the rest of the shader bindings
                    lineShader->setUniformValue(lineShader->model(), modelMatrix);
                    lineShader->setUniformValue(lineShader->nModel(),
                                                itModelMatrix.inverted().transposed());
                    lineShader->setUniformValue(lineShader->MVP(), MVPMatrix);

#if !defined(QT_OPENGL_ES_2)
                    if (m_cachedShadowQuality > QAbstract3DGraph::ShadowQualityNone) {
                        // Set shadow shader bindings
                        QMatrix4x4 depthMVPMatrix = depthProjectionViewMatrix * modelMatrix;
                        lineShader->setUniformValue(lineShader->depth(), depthMVPMatrix);
                        // Draw the object
                        m_drawer->drawObject(lineShader, m_gridLineObj, 0, m_depthTexture);
                    } else {
                        // Draw the object
                        m_drawer->drawObject(lineShader, m_gridLineObj);
                    }
#else
                    m_drawer->drawLine(lineShader);
#endif
                }

                // Back wall lines
                GLfloat lineZTrans = m_scaleZWithBackground - gridLineOffset;

                if (!m_zFlipped)
                    lineZTrans = -lineZTrans;

                for (int line = 0; line < gridLineCount; line++) {
                    QMatrix4x4 modelMatrix;
                    QMatrix4x4 MVPMatrix;
                    QMatrix4x4 itModelMatrix;

                    modelMatrix.translate(m_axisCacheX.gridLinePosition(line), 0.0f, lineZTrans);

                    modelMatrix.scale(gridLineScaleY);
                    itModelMatrix.scale(gridLineScaleY);

#if !defined(QT_OPENGL_ES_2)
                    if (m_zFlipped) {
                        modelMatrix.rotate(m_xFlipRotation);
                        itModelMatrix.rotate(m_xFlipRotation);
                    }
#else
                    modelMatrix.rotate(m_zRightAngleRotation);
                    itModelMatrix.rotate(m_zRightAngleRotation);
#endif

                    MVPMatrix = projectionViewMatrix * modelMatrix;

                    // Set the rest of the shader bindings
                    lineShader->setUniformValue(lineShader->model(), modelMatrix);
                    lineShader->setUniformValue(lineShader->nModel(),
                                                itModelMatrix.inverted().transposed());
                    lineShader->setUniformValue(lineShader->MVP(), MVPMatrix);

#if !defined(QT_OPENGL_ES_2)
                    if (m_cachedShadowQuality > QAbstract3DGraph::ShadowQualityNone) {
                        // Set shadow shader bindings
                        QMatrix4x4 depthMVPMatrix = depthProjectionViewMatrix * modelMatrix;
                        lineShader->setUniformValue(lineShader->depth(), depthMVPMatrix);
                        // Draw the object
                        m_drawer->drawObject(lineShader, m_gridLineObj, 0, m_depthTexture);
                    } else {
                        // Draw the object
                        m_drawer->drawObject(lineShader, m_gridLineObj);
                    }
#else
                    m_drawer->drawLine(lineShader);
#endif
                }
            }
        }

        // Horizontal wall lines
        if (m_axisCacheY.segmentCount() > 0) {
            // Back wall
            int gridLineCount = m_axisCacheY.gridLineCount();

            GLfloat lineZTrans = m_scaleZWithBackground - gridLineOffset;

            if (!m_zFlipped)
                lineZTrans = -lineZTrans;

            for (int line = 0; line < gridLineCount; line++) {
                QMatrix4x4 modelMatrix;
                QMatrix4x4 MVPMatrix;
                QMatrix4x4 itModelMatrix;

                modelMatrix.translate(0.0f, m_axisCacheY.gridLinePosition(line), lineZTrans);

                modelMatrix.scale(gridLineScaleX);
                itModelMatrix.scale(gridLineScaleX);

                if (m_zFlipped) {
                    modelMatrix.rotate(m_xFlipRotation);
                    itModelMatrix.rotate(m_xFlipRotation);
                }

                MVPMatrix = projectionViewMatrix * modelMatrix;

                // Set the rest of the shader bindings
                lineShader->setUniformValue(lineShader->model(), modelMatrix);
                lineShader->setUniformValue(lineShader->nModel(),
                                            itModelMatrix.inverted().transposed());
                lineShader->setUniformValue(lineShader->MVP(), MVPMatrix);

#if !defined(QT_OPENGL_ES_2)
                if (m_cachedShadowQuality > QAbstract3DGraph::ShadowQualityNone) {
                    // Set shadow shader bindings
                    QMatrix4x4 depthMVPMatrix = depthProjectionViewMatrix * modelMatrix;
                    lineShader->setUniformValue(lineShader->depth(), depthMVPMatrix);
                    // Draw the object
                    m_drawer->drawObject(lineShader, m_gridLineObj, 0, m_depthTexture);
                } else {
                    // Draw the object
                    m_drawer->drawObject(lineShader, m_gridLineObj);
                }
#else
                m_drawer->drawLine(lineShader);
#endif
            }

            // Side wall
            GLfloat lineXTrans = m_scaleXWithBackground - gridLineOffset;

            if (!m_xFlipped)
                lineXTrans = -lineXTrans;

            for (int line = 0; line < gridLineCount; line++) {
                QMatrix4x4 modelMatrix;
                QMatrix4x4 MVPMatrix;
                QMatrix4x4 itModelMatrix;

                modelMatrix.translate(lineXTrans, m_axisCacheY.gridLinePosition(line), 0.0f);

                modelMatrix.scale(gridLineScaleZ);
                itModelMatrix.scale(gridLineScaleZ);

                modelMatrix.rotate(lineYRotation);
                itModelMatrix.rotate(lineYRotation);

                MVPMatrix = projectionViewMatrix * modelMatrix;

                // Set the rest of the shader bindings
                lineShader->setUniformValue(lineShader->model(), modelMatrix);
                lineShader->setUniformValue(lineShader->nModel(),
                                            itModelMatrix.inverted().transposed());
                lineShader->setUniformValue(lineShader->MVP(), MVPMatrix);

#if !defined(QT_OPENGL_ES_2)
                if (m_cachedShadowQuality > QAbstract3DGraph::ShadowQualityNone) {
                    // Set shadow shader bindings
                    QMatrix4x4 depthMVPMatrix = depthProjectionViewMatrix * modelMatrix;
                    lineShader->setUniformValue(lineShader->depth(), depthMVPMatrix);
                    // Draw the object
                    m_drawer->drawObject(lineShader, m_gridLineObj, 0, m_depthTexture);
                } else {
                    // Draw the object
                    m_drawer->drawObject(lineShader, m_gridLineObj);
                }
#else
                m_drawer->drawLine(lineShader);
#endif
            }
        }
    }

    Abstract3DRenderer::drawCustomItems(RenderingNormal, m_customItemShader, viewMatrix,
                                        projectionViewMatrix, depthProjectionViewMatrix,
                                        m_depthTexture, m_shadowQualityToShader);

    drawLabels(false, activeCamera, viewMatrix, projectionMatrix);

    // Release shader
    glUseProgram(0);

    // Selection handling
    if (m_selectionDirty || m_selectionLabelDirty) {
        QPoint visiblePoint = Surface3DController::invalidSelectionPosition();
        if (m_selectedSeries) {
            SurfaceSeriesRenderCache *cache =
                    static_cast<SurfaceSeriesRenderCache *>(
                        m_renderCacheList.value(const_cast<QSurface3DSeries *>(m_selectedSeries)));
            if (cache && m_selectedPoint != Surface3DController::invalidSelectionPosition()) {
                const QRect &sampleSpace = cache->sampleSpace();
                int x = m_selectedPoint.x() - sampleSpace.y();
                int y = m_selectedPoint.y() - sampleSpace.x();
                if (x >= 0 && y >= 0 && x < sampleSpace.height() && y < sampleSpace.width()
                        && cache->dataArray().size()) {
                    visiblePoint = QPoint(x, y);
                }
            }
        }

        if (m_cachedSelectionMode == QAbstract3DGraph::SelectionNone
                || visiblePoint == Surface3DController::invalidSelectionPosition()) {
            m_selectionActive = false;
        } else {
            if (m_cachedIsSlicingActivated)
                updateSliceDataModel(visiblePoint);
            if (m_cachedSelectionMode.testFlag(QAbstract3DGraph::SelectionItem))
                surfacePointSelected(visiblePoint);
            m_selectionActive = true;
        }

        m_selectionDirty = false;
    }
}

void Surface3DRenderer::drawLabels(bool drawSelection, const Q3DCamera *activeCamera,
                                   const QMatrix4x4 &viewMatrix,
                                   const QMatrix4x4 &projectionMatrix)
{
    ShaderHelper *shader = 0;
    GLfloat alphaForValueSelection = labelValueAlpha / 255.0f;
    GLfloat alphaForRowSelection = labelRowAlpha / 255.0f;
    GLfloat alphaForColumnSelection = labelColumnAlpha / 255.0f;
    if (drawSelection) {
        shader = m_surfaceGridShader;
    } else {
        shader = m_labelShader;
        shader->bind();
        glEnable(GL_TEXTURE_2D);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }

    glEnable(GL_POLYGON_OFFSET_FILL);

    float labelAutoAngle = m_axisCacheZ.labelAutoRotation();
    float labelAngleFraction = labelAutoAngle / 90.0f;
    float fractionCamY = activeCamera->yRotation() * labelAngleFraction;
    float fractionCamX = activeCamera->xRotation() * labelAngleFraction;
    float labelsMaxWidth = 0.0f;

    int startIndex;
    int endIndex;
    int indexStep;

    // Z Labels
    QVector3D positionZComp(0.0f, 0.0f, 0.0f);
    if (m_axisCacheZ.segmentCount() > 0) {
        int labelCount = m_axisCacheZ.labelCount();
        float labelXTrans = 0.0f;
        float labelYTrans = m_flipHorizontalGrid ? backgroundMargin : -backgroundMargin;
        if (m_polarGraph) {
            // TODO optional placement of radial labels - YTrans up only if over background
            labelXTrans = m_scaleXWithBackground + labelMargin;
            //labelYTrans += gridLineOffset + gridLineWidth;
        } else {
            labelXTrans = m_scaleXWithBackground + labelMargin;
        }
        Qt::AlignmentFlag alignment = (m_xFlipped == m_zFlipped) ? Qt::AlignLeft : Qt::AlignRight;
        QVector3D labelRotation;
        if (m_xFlipped)
            labelXTrans = -labelXTrans;
        if (m_yFlipped)
            labelYTrans = -labelYTrans;
        if (labelAutoAngle == 0.0f) {
            if (m_zFlipped)
                labelRotation.setY(180.0f);
            if (m_yFlippedForGrid) {
                if (m_zFlipped)
                    labelRotation.setY(180.0f);
                else
                    labelRotation.setY(0.0f);
                labelRotation.setX(90.0f);
            } else {
                labelRotation.setX(-90.0f);
            }
        } else {
            if (m_zFlipped)
                labelRotation.setY(180.0f);
            if (m_yFlippedForGrid) {
                if (m_zFlipped) {
                    if (m_xFlipped) {
                        labelRotation.setX(90.0f - (labelAutoAngle - fractionCamX)
                                           * (-labelAutoAngle - fractionCamY) / labelAutoAngle);
                        labelRotation.setZ(labelAutoAngle + fractionCamY);
                    } else {
                        labelRotation.setX(90.0f + (labelAutoAngle + fractionCamX)
                                           * (labelAutoAngle + fractionCamY) / labelAutoAngle);
                        labelRotation.setZ(-labelAutoAngle - fractionCamY);
                    }
                } else {
                    if (m_xFlipped) {
                        labelRotation.setX(90.0f + (labelAutoAngle - fractionCamX)
                                           * -(labelAutoAngle + fractionCamY) / labelAutoAngle);
                        labelRotation.setZ(-labelAutoAngle - fractionCamY);
                    } else {
                        labelRotation.setX(90.0f - (labelAutoAngle + fractionCamX)
                                           * (labelAutoAngle + fractionCamY) / labelAutoAngle);
                        labelRotation.setZ(labelAutoAngle + fractionCamY);
                    }
                }
            } else {
                if (m_zFlipped) {
                    if (m_xFlipped) {
                        labelRotation.setX(-90.0f + (labelAutoAngle - fractionCamX)
                                           * (-labelAutoAngle + fractionCamY) / labelAutoAngle);
                        labelRotation.setZ(-labelAutoAngle + fractionCamY);
                    } else {
                        labelRotation.setX(-90.0f - (labelAutoAngle + fractionCamX)
                                           * (labelAutoAngle - fractionCamY) / labelAutoAngle);
                        labelRotation.setZ(labelAutoAngle - fractionCamY);
                    }
                } else {
                    if (m_xFlipped) {
                        labelRotation.setX(-90.0f - (labelAutoAngle - fractionCamX)
                                           * (-labelAutoAngle + fractionCamY) / labelAutoAngle);
                        labelRotation.setZ(labelAutoAngle - fractionCamY);
                    } else {
                        labelRotation.setX(-90.0f + (labelAutoAngle + fractionCamX)
                                           * (labelAutoAngle - fractionCamY) / labelAutoAngle);
                        labelRotation.setZ(-labelAutoAngle + fractionCamY);
                    }
                }
            }
        }

        QQuaternion totalRotation = Utils::calculateRotation(labelRotation);

        QVector3D labelTrans = QVector3D(labelXTrans,
                                         labelYTrans,
                                         0.0f);

        if (m_zFlipped) {
            startIndex = 0;
            endIndex = labelCount;
            indexStep = 1;
        } else {
            startIndex = labelCount - 1;
            endIndex = -1;
            indexStep = -1;
        }
        float offsetValue = 0.0f;
        for (int label = startIndex; label != endIndex; label = label + indexStep) {
            glPolygonOffset(offsetValue++ / -10.0f, 1.0f);
            const LabelItem &axisLabelItem = *m_axisCacheZ.labelItems().at(label);
            // Draw the label here
            if (m_polarGraph) {
                float direction = m_zFlipped ? -1.0f : 1.0f;
                labelTrans.setZ((m_axisCacheZ.formatter()->labelPositions().at(label)
                                * -m_graphAspectRatio
                                + m_drawer->scaledFontSize() + gridLineWidth) * direction);
            } else {
                labelTrans.setZ(m_axisCacheZ.labelPosition(label));
            }
            m_dummyRenderItem.setTranslation(labelTrans);

            if (drawSelection) {
                QVector4D labelColor = QVector4D(label / 255.0f, 0.0f, 0.0f,
                                                 alphaForRowSelection);
                shader->setUniformValue(shader->color(), labelColor);
            }

            m_drawer->drawLabel(m_dummyRenderItem, axisLabelItem, viewMatrix, projectionMatrix,
                                positionZComp, totalRotation, 0, m_cachedSelectionMode,
                                shader, m_labelObj, activeCamera,
                                true, true, Drawer::LabelMid, alignment, false, drawSelection);
            labelsMaxWidth = qMax(labelsMaxWidth, float(axisLabelItem.size().width()));
        }
        if (!drawSelection && m_axisCacheZ.isTitleVisible()) {
            labelTrans.setZ(0.0f);
            drawAxisTitleZ(labelRotation, labelTrans, totalRotation, m_dummyRenderItem,
                           activeCamera, labelsMaxWidth, viewMatrix, projectionMatrix, shader);
        }
    }
    // X Labels
    if (m_axisCacheX.segmentCount() > 0) {
        labelsMaxWidth = 0.0f;
        labelAutoAngle = m_axisCacheX.labelAutoRotation();
        labelAngleFraction = labelAutoAngle / 90.0f;
        fractionCamY = activeCamera->yRotation() * labelAngleFraction;
        fractionCamX = activeCamera->xRotation() * labelAngleFraction;
        int labelCount = m_axisCacheX.labelCount();

        GLfloat labelZTrans = 0.0f;
        float labelYTrans = m_flipHorizontalGrid ? backgroundMargin : -backgroundMargin;
        if (m_polarGraph)
            labelYTrans += gridLineOffset + gridLineWidth;
        else
            labelZTrans = m_scaleZWithBackground + labelMargin;

        Qt::AlignmentFlag alignment = (m_xFlipped != m_zFlipped) ? Qt::AlignLeft : Qt::AlignRight;
        QVector3D labelRotation;
        if (m_zFlipped)
            labelZTrans = -labelZTrans;
        if (m_yFlipped)
            labelYTrans = -labelYTrans;
        if (labelAutoAngle == 0.0f) {
            labelRotation = QVector3D(-90.0f, 90.0f, 0.0f);
            if (m_xFlipped)
                labelRotation.setY(-90.0f);
            if (m_yFlippedForGrid) {
                if (m_xFlipped)
                    labelRotation.setY(-90.0f);
                else
                    labelRotation.setY(90.0f);
                labelRotation.setX(90.0f);
            }
        } else {
            if (m_xFlipped)
                labelRotation.setY(-90.0f);
            else
                labelRotation.setY(90.0f);
            if (m_yFlippedForGrid) {
                if (m_zFlipped) {
                    if (m_xFlipped) {
                        labelRotation.setX(90.0f - (2.0f * labelAutoAngle - fractionCamX)
                                           * (labelAutoAngle + fractionCamY) / labelAutoAngle);
                        labelRotation.setZ(-labelAutoAngle - fractionCamY);
                    } else {
                        labelRotation.setX(90.0f - (2.0f * labelAutoAngle + fractionCamX)
                                           * (labelAutoAngle + fractionCamY) / labelAutoAngle);
                        labelRotation.setZ(labelAutoAngle + fractionCamY);
                    }
                } else {
                    if (m_xFlipped) {
                        labelRotation.setX(90.0f + fractionCamX
                                           * -(labelAutoAngle + fractionCamY) / labelAutoAngle);
                        labelRotation.setZ(labelAutoAngle + fractionCamY);
                    } else {
                        labelRotation.setX(90.0f - fractionCamX
                                           * (-labelAutoAngle - fractionCamY) / labelAutoAngle);
                        labelRotation.setZ(-labelAutoAngle - fractionCamY);
                    }
                }
            } else {
                if (m_zFlipped) {
                    if (m_xFlipped) {
                        labelRotation.setX(-90.0f + (2.0f * labelAutoAngle - fractionCamX)
                                           * (labelAutoAngle - fractionCamY) / labelAutoAngle);
                        labelRotation.setZ(labelAutoAngle - fractionCamY);
                    } else {
                        labelRotation.setX(-90.0f + (2.0f * labelAutoAngle + fractionCamX)
                                           * (labelAutoAngle - fractionCamY) / labelAutoAngle);
                        labelRotation.setZ(-labelAutoAngle + fractionCamY);
                    }
                } else {
                    if (m_xFlipped) {
                        labelRotation.setX(-90.0f - fractionCamX
                                           * (-labelAutoAngle + fractionCamY) / labelAutoAngle);
                        labelRotation.setZ(-labelAutoAngle + fractionCamY);
                    } else {
                        labelRotation.setX(-90.0f + fractionCamX
                                           * -(labelAutoAngle - fractionCamY) / labelAutoAngle);
                        labelRotation.setZ(labelAutoAngle - fractionCamY);
                    }
                }
            }
        }

        QQuaternion totalRotation = Utils::calculateRotation(labelRotation);
        if (m_polarGraph) {
            if (!m_yFlippedForGrid && (m_zFlipped != m_xFlipped)
                    || m_yFlippedForGrid && (m_zFlipped == m_xFlipped)) {
                totalRotation *= m_zRightAngleRotation;
            } else {
                totalRotation *= m_zRightAngleRotationNeg;
            }
        }

        QVector3D labelTrans = QVector3D(0.0f,
                                         labelYTrans,
                                         labelZTrans);

        if (m_xFlipped) {
            startIndex = labelCount - 1;
            endIndex = -1;
            indexStep = -1;
        } else {
            startIndex = 0;
            endIndex = labelCount;
            indexStep = 1;
        }
        float offsetValue = 0.0f;
        bool showLastLabel = false;
        QVector<float> &gridPositions = m_axisCacheX.formatter()->gridPositions();
        int lastGridPosIndex = gridPositions.size() - 1;
        if (gridPositions.size()
                && (gridPositions.at(lastGridPosIndex) != 1.0f || gridPositions.at(0) != 0.0f)) {
            // Avoid overlapping first and last label if they would get on same position
            showLastLabel = true;
        }

        for (int label = startIndex; label != endIndex; label = label + indexStep) {
            glPolygonOffset(offsetValue++ / -10.0f, 1.0f);
            // Draw the label here
            if (m_polarGraph) {
                // Calculate angular position
                if (label == lastGridPosIndex && !showLastLabel)
                    continue;
                float gridPosition = m_axisCacheX.formatter()->gridPositions().at(label);
                qreal angle = gridPosition * M_PI * 2.0;
                labelTrans.setX((m_graphAspectRatio + labelMargin)* float(qSin(angle)));
                labelTrans.setZ(-(m_graphAspectRatio + labelMargin) * float(qCos(angle)));
                // Alignment depends on label angular position, as well as flips
                Qt::AlignmentFlag vAlignment = Qt::AlignCenter;
                Qt::AlignmentFlag hAlignment = Qt::AlignCenter;
                const float centerMargin = 0.005f;
                if (gridPosition < 0.25f - centerMargin || gridPosition > 0.75f + centerMargin)
                    vAlignment = m_zFlipped ? Qt::AlignTop : Qt::AlignBottom;
                else if (gridPosition > 0.25f + centerMargin && gridPosition < 0.75f - centerMargin)
                    vAlignment = m_zFlipped ? Qt::AlignBottom : Qt::AlignTop;

                if (gridPosition < 0.50f - centerMargin && gridPosition > centerMargin)
                    hAlignment = m_zFlipped ? Qt::AlignRight : Qt::AlignLeft;
                else if (gridPosition < 1.0f - centerMargin && gridPosition > 0.5f + centerMargin)
                    hAlignment = m_zFlipped ? Qt::AlignLeft : Qt::AlignRight;
                if (m_yFlippedForGrid && vAlignment != Qt::AlignCenter)
                    vAlignment = (vAlignment == Qt::AlignTop) ? Qt::AlignBottom : Qt::AlignTop;
                alignment = Qt::AlignmentFlag(vAlignment | hAlignment);
            } else {
                labelTrans.setX(m_axisCacheX.labelPosition(label));
            }
            m_dummyRenderItem.setTranslation(labelTrans);
            const LabelItem &axisLabelItem = *m_axisCacheX.labelItems().at(label);

            if (drawSelection) {
                QVector4D labelColor = QVector4D(0.0f, label / 255.0f, 0.0f,
                                                 alphaForColumnSelection);
                shader->setUniformValue(shader->color(), labelColor);
            }

            m_drawer->drawLabel(m_dummyRenderItem, axisLabelItem, viewMatrix, projectionMatrix,
                                positionZComp, totalRotation, 0, m_cachedSelectionMode,
                                shader, m_labelObj, activeCamera,
                                true, true, Drawer::LabelMid, alignment, false, drawSelection);
            labelsMaxWidth = qMax(labelsMaxWidth, float(axisLabelItem.size().width()));
        }
        if (!drawSelection && m_axisCacheX.isTitleVisible()) {
            labelTrans.setX(0.0f);
            drawAxisTitleX(labelRotation, labelTrans, totalRotation, m_dummyRenderItem,
                           activeCamera, labelsMaxWidth, viewMatrix, projectionMatrix, shader);
        }
    }
    // Y Labels
    if (m_axisCacheY.segmentCount() > 0) {
        labelsMaxWidth = 0.0f;
        labelAutoAngle = m_axisCacheY.labelAutoRotation();
        labelAngleFraction = labelAutoAngle / 90.0f;
        fractionCamY = activeCamera->yRotation() * labelAngleFraction;
        fractionCamX = activeCamera->xRotation() * labelAngleFraction;
        int labelCount = m_axisCacheY.labelCount();

        GLfloat labelXTrans = m_scaleXWithBackground;
        GLfloat labelZTrans = m_scaleZWithBackground;

        // Back & side wall
        GLfloat labelMarginXTrans = labelMargin;
        GLfloat labelMarginZTrans = labelMargin;
        QVector3D backLabelRotation(0.0f, -90.0f, 0.0f);
        QVector3D sideLabelRotation(0.0f, 0.0f, 0.0f);
        Qt::AlignmentFlag backAlignment =
                (m_xFlipped != m_zFlipped) ? Qt::AlignLeft : Qt::AlignRight;
        Qt::AlignmentFlag sideAlignment =
                (m_xFlipped == m_zFlipped) ? Qt::AlignLeft : Qt::AlignRight;
        if (!m_xFlipped) {
            labelXTrans = -labelXTrans;
            labelMarginXTrans = -labelMargin;
        }
        if (m_zFlipped) {
            labelZTrans = -labelZTrans;
            labelMarginZTrans = -labelMargin;
        }
        if (labelAutoAngle == 0.0f) {
            if (!m_xFlipped)
                backLabelRotation.setY(90.0f);
            if (m_zFlipped)
                sideLabelRotation.setY(180.f);
        } else {
            // Orient side labels somewhat towards the camera
            if (m_xFlipped) {
                if (m_zFlipped)
                    sideLabelRotation.setY(180.0f + (2.0f * labelAutoAngle) - fractionCamX);
                else
                    sideLabelRotation.setY(-fractionCamX);
                backLabelRotation.setY(-90.0f + labelAutoAngle - fractionCamX);
            } else {
                if (m_zFlipped)
                    sideLabelRotation.setY(180.0f - (2.0f * labelAutoAngle) - fractionCamX);
                else
                    sideLabelRotation.setY(-fractionCamX);
                backLabelRotation.setY(90.0f - labelAutoAngle - fractionCamX);
            }
        }
        sideLabelRotation.setX(-fractionCamY);
        backLabelRotation.setX(-fractionCamY);

        QQuaternion totalSideRotation = Utils::calculateRotation(sideLabelRotation);
        QQuaternion totalBackRotation = Utils::calculateRotation(backLabelRotation);

        QVector3D labelTransBack = QVector3D(labelXTrans, 0.0f, labelZTrans + labelMarginZTrans);
        QVector3D labelTransSide(-labelXTrans - labelMarginXTrans, 0.0f, -labelZTrans);

        if (m_yFlipped) {
            startIndex = labelCount - 1;
            endIndex = -1;
            indexStep = -1;
        } else {
            startIndex = 0;
            endIndex = labelCount;
            indexStep = 1;
        }
        float offsetValue = 0.0f;
        for (int label = startIndex; label != endIndex; label = label + indexStep) {
            const LabelItem &axisLabelItem = *m_axisCacheY.labelItems().at(label);
            const GLfloat labelYTrans = m_axisCacheY.labelPosition(label);

            glPolygonOffset(offsetValue++ / -10.0f, 1.0f);

            if (drawSelection) {
                QVector4D labelColor = QVector4D(0.0f, 0.0f, label / 255.0f,
                                                 alphaForValueSelection);
                shader->setUniformValue(shader->color(), labelColor);
            }

            // Back wall
            labelTransBack.setY(labelYTrans);
            m_dummyRenderItem.setTranslation(labelTransBack);
            m_drawer->drawLabel(m_dummyRenderItem, axisLabelItem, viewMatrix, projectionMatrix,
                                positionZComp, totalBackRotation, 0, m_cachedSelectionMode,
                                shader, m_labelObj, activeCamera,
                                true, true, Drawer::LabelMid, backAlignment, false,
                                drawSelection);

            // Side wall
            labelTransSide.setY(labelYTrans);
            m_dummyRenderItem.setTranslation(labelTransSide);
            m_drawer->drawLabel(m_dummyRenderItem, axisLabelItem, viewMatrix, projectionMatrix,
                                positionZComp, totalSideRotation, 0, m_cachedSelectionMode,
                                shader, m_labelObj, activeCamera,
                                true, true, Drawer::LabelMid, sideAlignment, false,
                                drawSelection);
            labelsMaxWidth = qMax(labelsMaxWidth, float(axisLabelItem.size().width()));
        }
        if (!drawSelection && m_axisCacheY.isTitleVisible()) {
            labelTransSide.setY(0.0f);
            labelTransBack.setY(0.0f);
            drawAxisTitleY(sideLabelRotation, backLabelRotation, labelTransSide, labelTransBack,
                           totalSideRotation, totalBackRotation, m_dummyRenderItem, activeCamera,
                           labelsMaxWidth, viewMatrix, projectionMatrix,
                           shader);
        }
    }
    glDisable(GL_POLYGON_OFFSET_FILL);

    if (!drawSelection) {
        glDisable(GL_TEXTURE_2D);
        glDisable(GL_BLEND);
    }
}

void Surface3DRenderer::updateSelectionMode(QAbstract3DGraph::SelectionFlags mode)
{
    Abstract3DRenderer::updateSelectionMode(mode);

    if (m_cachedSelectionMode > QAbstract3DGraph::SelectionNone)
        updateSelectionTextures();
}

void Surface3DRenderer::updateSelectionTextures()
{
    uint lastSelectionId = 1;

    foreach (SeriesRenderCache *baseCache, m_renderCacheList) {
        SurfaceSeriesRenderCache *cache =
                static_cast<SurfaceSeriesRenderCache *>(baseCache);
        GLuint texture = cache->selectionTexture();
        m_textureHelper->deleteTexture(&texture);
        createSelectionTexture(cache, lastSelectionId);
    }
    m_selectionTexturesDirty = false;
}

void Surface3DRenderer::createSelectionTexture(SurfaceSeriesRenderCache *cache,
                                               uint &lastSelectionId)
{
    // Create the selection ID image. Each grid corner gets 2x2 pixel area of
    // ID color so that each vertex (data point) has 4x4 pixel area of ID color
    const QRect &sampleSpace = cache->sampleSpace();
    int idImageWidth = (sampleSpace.width() - 1) * 4;
    int idImageHeight = (sampleSpace.height() - 1) * 4;

    if (idImageHeight <= 0 || idImageWidth <= 0) {
        cache->setSelectionIdRange(-1, -1);
        cache->setSelectionTexture(0);
        return;
    }

    int stride = idImageWidth * 4 * sizeof(uchar); // 4 = number of color components (rgba)

    uint idStart = lastSelectionId;
    uchar *bits = new uchar[idImageWidth * idImageHeight * 4 * sizeof(uchar)];
    for (int i = 0; i < idImageHeight; i += 4) {
        for (int j = 0; j < idImageWidth; j += 4) {
            int p = (i * idImageWidth + j) * 4;
            uchar r, g, b, a;
            idToRGBA(lastSelectionId, &r, &g, &b, &a);
            fillIdCorner(&bits[p], r, g, b, a, stride);

            idToRGBA(lastSelectionId + 1, &r, &g, &b, &a);
            fillIdCorner(&bits[p + 8], r, g, b, a, stride);

            idToRGBA(lastSelectionId + sampleSpace.width(), &r, &g, &b, &a);
            fillIdCorner(&bits[p + 2 * stride], r, g, b, a, stride);

            idToRGBA(lastSelectionId + sampleSpace.width() + 1, &r, &g, &b, &a);
            fillIdCorner(&bits[p + 2 * stride + 8], r, g, b, a, stride);

            lastSelectionId++;
        }
        lastSelectionId++;
    }
    lastSelectionId += sampleSpace.width();
    cache->setSelectionIdRange(idStart, lastSelectionId - 1);

    // Move the ID image (bits) to the texture
    QImage image = QImage(bits, idImageWidth, idImageHeight, QImage::Format_RGB32);
    GLuint selectionTexture = m_textureHelper->create2DTexture(image, false, false, false);
    cache->setSelectionTexture(selectionTexture);

    // Release the temp bits allocation
    delete[] bits;
}

void Surface3DRenderer::initSelectionBuffer()
{
    // Create the result selection texture and buffers
    m_textureHelper->deleteTexture(&m_selectionResultTexture);

    m_selectionResultTexture = m_textureHelper->createSelectionTexture(m_primarySubViewport.size(),
                                                                       m_selectionFrameBuffer,
                                                                       m_selectionDepthBuffer);
}

void Surface3DRenderer::fillIdCorner(uchar *p, uchar r, uchar g, uchar b, uchar a, int stride)
{
    p[0] = r;
    p[1] = g;
    p[2] = b;
    p[3] = a;
    p[4] = r;
    p[5] = g;
    p[6] = b;
    p[7] = a;
    p[stride + 0] = r;
    p[stride + 1] = g;
    p[stride + 2] = b;
    p[stride + 3] = a;
    p[stride + 4] = r;
    p[stride + 5] = g;
    p[stride + 6] = b;
    p[stride + 7] = a;
}

void Surface3DRenderer::idToRGBA(uint id, uchar *r, uchar *g, uchar *b, uchar *a)
{
    *r = id & ID_TO_RGBA_MASK;
    *g = (id >> 8) & ID_TO_RGBA_MASK;
    *b = (id >> 16) & ID_TO_RGBA_MASK;
    *a = (id >> 24) & ID_TO_RGBA_MASK;
}

void Surface3DRenderer::calculateSceneScalingFactors()
{
    // Calculate scene scaling and translation factors
    m_heightNormalizer = GLfloat(m_axisCacheY.max() - m_axisCacheY.min());
    m_areaSize.setHeight(m_axisCacheZ.max() -  m_axisCacheZ.min());
    m_areaSize.setWidth(m_axisCacheX.max() - m_axisCacheX.min());
    float scaleFactor = qMax(m_areaSize.width(), m_areaSize.height());
    if (m_polarGraph) {
        m_scaleX = m_graphAspectRatio;
        m_scaleZ = m_graphAspectRatio;
    } else {
        m_scaleX = m_graphAspectRatio * m_areaSize.width() / scaleFactor;
        m_scaleZ = m_graphAspectRatio * m_areaSize.height() / scaleFactor;
    }
    m_scaleXWithBackground = m_scaleX + backgroundMargin - 1.0f;
    m_scaleZWithBackground = m_scaleZ + backgroundMargin - 1.0f;

    float factorScaler = 2.0f * m_graphAspectRatio / scaleFactor;
    m_axisCacheX.setScale(factorScaler * m_areaSize.width());
    m_axisCacheZ.setScale(-factorScaler * m_areaSize.height());
    m_axisCacheX.setTranslate(-m_axisCacheX.scale() / 2.0f);
    m_axisCacheZ.setTranslate(-m_axisCacheZ.scale() / 2.0f);
}

void Surface3DRenderer::checkFlatSupport(SurfaceSeriesRenderCache *cache)
{
    bool flatEnable = cache->isFlatShadingEnabled();
    if (flatEnable && !m_flatSupported) {
        qWarning() << "Warning: Flat qualifier not supported on your platform's GLSL language."
                      " Requires at least GLSL version 1.2 with GL_EXT_gpu_shader4 extension.";
        cache->setFlatShadingEnabled(false);
        cache->setFlatChangeAllowed(false);
    }
}

void Surface3DRenderer::updateObjects(SurfaceSeriesRenderCache *cache, bool dimensionChanged)
{
    QSurfaceDataArray &dataArray = cache->dataArray();
    const QRect &sampleSpace = cache->sampleSpace();

    if (cache->isFlatShadingEnabled()) {
        cache->surfaceObject()->setUpData(dataArray, sampleSpace, dimensionChanged, m_polarGraph);
    } else {
        cache->surfaceObject()->setUpSmoothData(dataArray, sampleSpace, dimensionChanged,
                                                m_polarGraph);
    }
}

void Surface3DRenderer::updateSelectedPoint(const QPoint &position, QSurface3DSeries *series)
{
    m_selectedPoint = position;
    m_selectedSeries = series;
    m_selectionDirty = true;
}

void Surface3DRenderer::updateFlipHorizontalGrid(bool flip)
{
    m_flipHorizontalGrid = flip;
}

void Surface3DRenderer::resetClickedStatus()
{
    m_clickedPosition = Surface3DController::invalidSelectionPosition();
    m_clickedSeries = 0;
}

void Surface3DRenderer::loadBackgroundMesh()
{
    ObjectHelper::resetObjectHelper(this, m_backgroundObj,
                                    QStringLiteral(":/defaultMeshes/background"));
}

void Surface3DRenderer::surfacePointSelected(const QPoint &point)
{
    foreach (SeriesRenderCache *baseCache, m_renderCacheList) {
        SurfaceSeriesRenderCache *cache =
                static_cast<SurfaceSeriesRenderCache *>(baseCache);
        cache->setSlicePointerActivity(false);
        cache->setMainPointerActivity(false);
    }

    if (m_cachedSelectionMode.testFlag(QAbstract3DGraph::SelectionMultiSeries)) {
        // Find axis coordinates for the selected point
        SurfaceSeriesRenderCache *selectedCache =
                static_cast<SurfaceSeriesRenderCache *>(
                    m_renderCacheList.value(const_cast<QSurface3DSeries *>(m_selectedSeries)));
        QSurfaceDataArray &dataArray = selectedCache->dataArray();
        QSurfaceDataItem item = dataArray.at(point.x())->at(point.y());
        QPointF coords(item.x(), item.z());

        foreach (SeriesRenderCache *baseCache, m_renderCacheList) {
            SurfaceSeriesRenderCache *cache =
                    static_cast<SurfaceSeriesRenderCache *>(baseCache);
            if (cache->series() != m_selectedSeries) {
                QPoint mappedPoint = mapCoordsToSampleSpace(cache, coords);
                updateSelectionPoint(cache, mappedPoint, false);
            } else {
                updateSelectionPoint(cache, point, true);
            }
        }
    } else {
        if (m_selectedSeries) {
            SurfaceSeriesRenderCache *cache =
                    static_cast<SurfaceSeriesRenderCache *>(
                        m_renderCacheList.value(const_cast<QSurface3DSeries *>(m_selectedSeries)));
            if (cache)
                updateSelectionPoint(cache, point, true);
        }
    }
}

void Surface3DRenderer::updateSelectionPoint(SurfaceSeriesRenderCache *cache, const QPoint &point,
                                             bool label)
{
    int row = point.x();
    int column = point.y();

    if (column < 0 || row < 0)
        return;

    SelectionPointer *slicePointer = cache->sliceSelectionPointer();
    if (!slicePointer && m_cachedIsSlicingActivated) {
        slicePointer = new SelectionPointer(m_drawer);
        cache->setSliceSelectionPointer(slicePointer);
    }
    SelectionPointer *mainPointer = cache->mainSelectionPointer();
    if (!mainPointer) {
        mainPointer = new SelectionPointer(m_drawer);
        cache->setMainSelectionPointer(mainPointer);
    }

    QString selectionLabel;
    if (label) {
        m_selectionLabelDirty = false;
        selectionLabel = cache->itemLabel();
    }

    if (m_cachedIsSlicingActivated) {
        QVector3D subPosFront;
        QVector3D subPosBack;
        if (m_cachedSelectionMode.testFlag(QAbstract3DGraph::SelectionRow)) {
            subPosFront = cache->sliceSurfaceObject()->vertexAt(column, 0);
            subPosBack = cache->sliceSurfaceObject()->vertexAt(column, 1);
        } else if (m_cachedSelectionMode.testFlag(QAbstract3DGraph::SelectionColumn)) {
            subPosFront = cache->sliceSurfaceObject()->vertexAt(row, 0);
            subPosBack = cache->sliceSurfaceObject()->vertexAt(row, 1);
        }
        slicePointer->updateBoundingRect(m_secondarySubViewport);
        slicePointer->updateSliceData(true, m_autoScaleAdjustment);
        slicePointer->setPosition((subPosFront + subPosBack) / 2.0f);
        slicePointer->setLabel(selectionLabel);
        slicePointer->setPointerObject(cache->object());
        slicePointer->setLabelObject(m_labelObj);
        slicePointer->setHighlightColor(cache->singleHighlightColor());
        slicePointer->updateScene(m_cachedScene);
        slicePointer->setRotation(cache->meshRotation());
        cache->setSlicePointerActivity(true);
    }

    QVector3D mainPos;
    mainPos = cache->surfaceObject()->vertexAt(column, row);
    mainPointer->updateBoundingRect(m_primarySubViewport);
    mainPointer->updateSliceData(false, m_autoScaleAdjustment);
    mainPointer->setPosition(mainPos);
    mainPointer->setLabel(selectionLabel);
    mainPointer->setPointerObject(cache->object());
    mainPointer->setLabelObject(m_labelObj);
    mainPointer->setHighlightColor(cache->singleHighlightColor());
    mainPointer->updateScene(m_cachedScene);
    mainPointer->setRotation(cache->meshRotation());
    cache->setMainPointerActivity(true);
}

// Maps selection Id to surface point in data array
QPoint Surface3DRenderer::selectionIdToSurfacePoint(uint id)
{
    m_clickedType = QAbstract3DGraph::ElementNone;
    m_selectedLabelIndex = -1;
    m_selectedCustomItemIndex = -1;
    // Check for label and custom item selection
    if (id / alphaMultiplier == labelRowAlpha) {
        m_selectedLabelIndex = id - (alphaMultiplier * uint(labelRowAlpha));
        m_clickedType = QAbstract3DGraph::ElementAxisZLabel;
        return Surface3DController::invalidSelectionPosition();
    } else if (id / alphaMultiplier == labelColumnAlpha) {
        m_selectedLabelIndex = (id - (alphaMultiplier * uint(labelColumnAlpha))) / greenMultiplier;
        m_clickedType = QAbstract3DGraph::ElementAxisXLabel;
        return Surface3DController::invalidSelectionPosition();
    } else if (id / alphaMultiplier == labelValueAlpha) {
        m_selectedLabelIndex = (id - (alphaMultiplier * uint(labelValueAlpha))) / blueMultiplier;
        m_clickedType = QAbstract3DGraph::ElementAxisYLabel;
        return Surface3DController::invalidSelectionPosition();
    } else if (id / alphaMultiplier == customItemAlpha) {
        // Custom item selection
        m_clickedType = QAbstract3DGraph::ElementCustomItem;
        m_selectedCustomItemIndex = id - (alphaMultiplier * uint(customItemAlpha));
        return Surface3DController::invalidSelectionPosition();
    }

    // Not a label selection
    SurfaceSeriesRenderCache *selectedCache = 0;
    foreach (SeriesRenderCache *baseCache, m_renderCacheList) {
        SurfaceSeriesRenderCache *cache = static_cast<SurfaceSeriesRenderCache *>(baseCache);
        if (cache->isWithinIdRange(id)) {
            selectedCache = cache;
            break;
        }
    }
    if (!selectedCache) {
        m_clickedSeries = 0;
        return Surface3DController::invalidSelectionPosition();
    }

    uint idInSeries = id - selectedCache->selectionIdStart() + 1;
    const QRect &sampleSpace = selectedCache->sampleSpace();
    int column = ((idInSeries - 1) % sampleSpace.width()) + sampleSpace.x();
    int row = ((idInSeries - 1) / sampleSpace.width()) +  sampleSpace.y();

    m_clickedSeries = selectedCache->series();
    m_clickedType = QAbstract3DGraph::ElementSeries;
    return QPoint(row, column);
}

void Surface3DRenderer::updateShadowQuality(QAbstract3DGraph::ShadowQuality quality)
{
    m_cachedShadowQuality = quality;

    switch (quality) {
    case QAbstract3DGraph::ShadowQualityLow:
        m_shadowQualityToShader = 33.3f;
        m_shadowQualityMultiplier = 1;
        break;
    case QAbstract3DGraph::ShadowQualityMedium:
        m_shadowQualityToShader = 100.0f;
        m_shadowQualityMultiplier = 3;
        break;
    case QAbstract3DGraph::ShadowQualityHigh:
        m_shadowQualityToShader = 200.0f;
        m_shadowQualityMultiplier = 5;
        break;
    case QAbstract3DGraph::ShadowQualitySoftLow:
        m_shadowQualityToShader = 5.0f;
        m_shadowQualityMultiplier = 1;
        break;
    case QAbstract3DGraph::ShadowQualitySoftMedium:
        m_shadowQualityToShader = 10.0f;
        m_shadowQualityMultiplier = 3;
        break;
    case QAbstract3DGraph::ShadowQualitySoftHigh:
        m_shadowQualityToShader = 15.0f;
        m_shadowQualityMultiplier = 4;
        break;
    default:
        m_shadowQualityToShader = 0.0f;
        m_shadowQualityMultiplier = 1;
        break;
    }

    handleShadowQualityChange();

#if !defined(QT_OPENGL_ES_2)
    updateDepthBuffer();
#endif
}

void Surface3DRenderer::updateTextures()
{
    // Do nothing, but required as it is pure virtual on parent
}

void Surface3DRenderer::updateSlicingActive(bool isSlicing)
{
    if (m_cachedIsSlicingActivated == isSlicing)
        return;

    m_cachedIsSlicingActivated = isSlicing;

    if (!m_cachedIsSlicingActivated)
        initSelectionBuffer(); // We need to re-init selection buffer in case there has been a resize

#if !defined(QT_OPENGL_ES_2)
    updateDepthBuffer(); // Re-init depth buffer as well
#endif

    m_selectionDirty = true;

    foreach (SeriesRenderCache *baseCache, m_renderCacheList) {
        SurfaceSeriesRenderCache *cache = static_cast<SurfaceSeriesRenderCache *>(baseCache);
        if (cache->mainSelectionPointer())
            cache->mainSelectionPointer()->updateBoundingRect(m_primarySubViewport);
    }
}

void Surface3DRenderer::initShaders(const QString &vertexShader, const QString &fragmentShader)
{
    Q_UNUSED(vertexShader);
    Q_UNUSED(fragmentShader);

    // draw the shader for the surface according to smooth status, shadow and uniform color
    if (m_surfaceFlatShader)
        delete m_surfaceFlatShader;
    if (m_surfaceSmoothShader)
        delete m_surfaceSmoothShader;
    if (m_surfaceSliceFlatShader)
        delete m_surfaceSliceFlatShader;
    if (m_surfaceSliceSmoothShader)
        delete m_surfaceSliceSmoothShader;

#if !defined(QT_OPENGL_ES_2)
    if (m_cachedShadowQuality > QAbstract3DGraph::ShadowQualityNone) {
        m_surfaceSmoothShader = new ShaderHelper(this, QStringLiteral(":/shaders/vertexShadow"),
                                                 QStringLiteral(":/shaders/fragmentSurfaceShadowNoTex"));
    } else {
        m_surfaceSmoothShader = new ShaderHelper(this, QStringLiteral(":/shaders/vertex"),
                                                 QStringLiteral(":/shaders/fragmentSurface"));
    }
    m_surfaceSliceSmoothShader = new ShaderHelper(this, QStringLiteral(":/shaders/vertex"),
                                                  QStringLiteral(":/shaders/fragmentSurface"));
    if (m_flatSupported) {
        if (m_cachedShadowQuality > QAbstract3DGraph::ShadowQualityNone) {
            m_surfaceFlatShader = new ShaderHelper(this, QStringLiteral(":/shaders/vertexSurfaceShadowFlat"),
                                                   QStringLiteral(":/shaders/fragmentSurfaceShadowFlat"));
        } else {
            m_surfaceFlatShader = new ShaderHelper(this, QStringLiteral(":/shaders/vertexSurfaceFlat"),
                                                   QStringLiteral(":/shaders/fragmentSurfaceFlat"));
        }
        m_surfaceSliceFlatShader = new ShaderHelper(this, QStringLiteral(":/shaders/vertexSurfaceFlat"),
                                                    QStringLiteral(":/shaders/fragmentSurfaceFlat"));
    } else {
        m_surfaceFlatShader = 0;
        m_surfaceSliceFlatShader = 0;
    }
#else
    m_surfaceSmoothShader = new ShaderHelper(this, QStringLiteral(":/shaders/vertex"),
                                             QStringLiteral(":/shaders/fragmentSurfaceES2"));
    m_surfaceFlatShader = new ShaderHelper(this, QStringLiteral(":/shaders/vertex"),
                                           QStringLiteral(":/shaders/fragmentSurfaceES2"));
    m_surfaceSliceSmoothShader = new ShaderHelper(this, QStringLiteral(":/shaders/vertex"),
                                                  QStringLiteral(":/shaders/fragmentSurfaceES2"));
    m_surfaceSliceFlatShader = new ShaderHelper(this, QStringLiteral(":/shaders/vertex"),
                                                QStringLiteral(":/shaders/fragmentSurfaceES2"));
#endif
    m_surfaceSmoothShader->initialize();
    m_surfaceSliceSmoothShader->initialize();
    if (m_flatSupported) {
        m_surfaceFlatShader->initialize();
        m_surfaceSliceFlatShader->initialize();
    }
}

void Surface3DRenderer::initBackgroundShaders(const QString &vertexShader,
                                              const QString &fragmentShader)
{
    if (m_backgroundShader)
        delete m_backgroundShader;
    m_backgroundShader = new ShaderHelper(this, vertexShader, fragmentShader);
    m_backgroundShader->initialize();
}

void Surface3DRenderer::initSelectionShaders()
{
    if (m_selectionShader)
        delete m_selectionShader;
    m_selectionShader = new ShaderHelper(this, QStringLiteral(":/shaders/vertexLabel"),
                                         QStringLiteral(":/shaders/fragmentLabel"));
    m_selectionShader->initialize();
}

void Surface3DRenderer::initSurfaceShaders()
{
    // Gridline shader
    if (m_surfaceGridShader)
        delete m_surfaceGridShader;
    m_surfaceGridShader = new ShaderHelper(this, QStringLiteral(":/shaders/vertexPlainColor"),
                                           QStringLiteral(":/shaders/fragmentPlainColor"));
    m_surfaceGridShader->initialize();

    // Triggers surface shader selection by shadow setting
    handleShadowQualityChange();
}

void Surface3DRenderer::initLabelShaders(const QString &vertexShader, const QString &fragmentShader)
{
    if (m_labelShader)
        delete m_labelShader;
    m_labelShader = new ShaderHelper(this, vertexShader, fragmentShader);
    m_labelShader->initialize();
}

#if !defined(QT_OPENGL_ES_2)
void Surface3DRenderer::initDepthShader()
{
    if (m_depthShader)
        delete m_depthShader;
    m_depthShader = new ShaderHelper(this, QStringLiteral(":/shaders/vertexDepth"),
                                     QStringLiteral(":/shaders/fragmentDepth"));
    m_depthShader->initialize();
}

void Surface3DRenderer::updateDepthBuffer()
{
    m_textureHelper->deleteTexture(&m_depthTexture);
    m_textureHelper->deleteTexture(&m_depthModelTexture);

    if (m_primarySubViewport.size().isEmpty())
        return;

    if (m_cachedShadowQuality > QAbstract3DGraph::ShadowQualityNone) {
        m_depthTexture = m_textureHelper->createDepthTextureFrameBuffer(m_primarySubViewport.size(),
                                                                        m_depthFrameBuffer,
                                                                        m_shadowQualityMultiplier);
        if (m_depthTexture) {
            m_textureHelper->fillDepthTexture(m_depthTexture, m_primarySubViewport.size(),
                                              m_shadowQualityMultiplier, 1.0f);
            m_depthModelTexture = m_textureHelper->createDepthTexture(m_primarySubViewport.size(),
                                                                      m_shadowQualityMultiplier);
        }
        if (!m_depthTexture || !m_depthModelTexture)
            lowerShadowQuality();
    }
}
#endif

QVector3D Surface3DRenderer::convertPositionToTranslation(const QVector3D &position,
                                                          bool isAbsolute)
{
    float xTrans = 0.0f;
    float yTrans = 0.0f;
    float zTrans = 0.0f;
    if (!isAbsolute) {
        xTrans = m_axisCacheX.positionAt(position.x());
        yTrans = m_axisCacheY.positionAt(position.y());
        zTrans = m_axisCacheZ.positionAt(position.z());
    } else {
        xTrans = position.x() * m_scaleX;
        yTrans = position.y();
        zTrans = position.z() * m_scaleZ;
    }
    return QVector3D(xTrans, yTrans, zTrans);
}

QT_END_NAMESPACE_DATAVISUALIZATION
