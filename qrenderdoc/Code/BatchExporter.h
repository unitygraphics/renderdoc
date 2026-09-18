/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2016-2026 Baldur Karlsson
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 ******************************************************************************/

#pragma once

#include <QString>
#include <QVector>
#include <QVector3D>
#include <QVector2D>
#include "Code/Interface/QRDInterface.h"

class BatchExporter
{
public:
  explicit BatchExporter(ICaptureContext &ctx);

  // Export textures and mesh bound in the current DrawCall's PipelineState.
  // Returns a human-readable status string. Must be called from a non-UI thread.
  QString exportDrawCallResources();

  // Export all textures and meshes across the entire capture.
  // Returns a human-readable status string. Must be called from a non-UI thread.
  QString exportAllResources();

private:
  struct PositionStream
  {
    int vbSlot = -1;
    uint32_t attrByteOffset = 0;    // offset of POSITION within each vertex
    uint8_t compCount = 0;
    uint8_t compByteWidth = 0;
    bool valid = false;
  };

  QList<ResourceId> collectDrawCallTextures();
  QList<ResourceId> collectAllTextures();
  int exportTextures(const QList<ResourceId> &ids, int &skipped);

  PositionStream findPositionStream();
  QVector<QVector3D> readVertexPositions(const PositionStream &pos,
                                         const BoundVBuffer &vb,
                                         uint32_t maxVerts,
                                         IReplayController *r);
  QVector<QVector3D> readVertexNormals(const PositionStream &posStream,
                                       const BoundVBuffer &vb,
                                       uint32_t maxVerts,
                                       IReplayController *r);
  QVector<QVector2D> readVertexUVs(const PositionStream &posStream,
                                   const BoundVBuffer &vb,
                                   uint32_t maxVerts,
                                   IReplayController *r);
  QVector<uint32_t> readIndexBuffer(const BoundVBuffer &ib, uint32_t numIndices,
                                    uint32_t baseVertex, IReplayController *r);
  bool writeObjFile(const QVector<QVector3D> &positions,
                    const QVector<QVector3D> &normals,
                    const QVector<QVector2D> &uvs,
                    const QVector<uint32_t> &indices,
                    ResourceId vbId,
                    uint32_t eventId);

  bool exportCurrentDrawCallMesh(int &skipped);
  void collectAllDrawcalls(const rdcarray<ActionDescription> &actions,
                           QVector<uint32_t> &outEventIds);

  void initExportDir();

  static QString sanitizeFileName(const QString &name);
  static QString resourceIdHex(ResourceId id);
  static bool isValidPositionFormat(const ResourceFormat &fmt);
  static void fillFromAttr(PositionStream &out, const VertexInputAttribute &attr);

  ICaptureContext &m_Ctx;
  QString m_ExportDir;

  static const QString kBaseDir;
};
