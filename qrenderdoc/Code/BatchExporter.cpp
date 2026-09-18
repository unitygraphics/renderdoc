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

#include "BatchExporter.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QTextStream>

const QString BatchExporter::kBaseDir = QStringLiteral("D:/Export");

BatchExporter::BatchExporter(ICaptureContext &ctx) : m_Ctx(ctx)
{
}

// ─────────────────────────── Utility ────────────────────────────────────────

void BatchExporter::initExportDir()
{
  QString ts = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd_HH-mm-ss"));
  m_ExportDir = kBaseDir + QLatin1Char('/') + ts;
  QDir().mkpath(m_ExportDir);
}

QString BatchExporter::sanitizeFileName(const QString &name)
{
  QString result = name;
  const QString illegal = QStringLiteral("\\/:*?\"<>|");
  for(QChar ch : illegal)
    result.replace(ch, QLatin1Char('_'));
  return result;
}

QString BatchExporter::resourceIdHex(ResourceId id)
{
  // ToStr yields "ResourceId::N" — use that as the unique identifier
  return ToQStr(id).replace(QLatin1Char(':'), QLatin1Char('_'));
}

// ─────────────────────────── Texture collection ─────────────────────────────

QList<ResourceId> BatchExporter::collectDrawCallTextures()
{
  QList<ResourceId> seen;
  QList<ResourceId> result;

  const PipeState &pipe = m_Ctx.CurPipelineState();

  const ShaderStage stages[] = {ShaderStage::Vertex,   ShaderStage::Hull,  ShaderStage::Domain,
                                 ShaderStage::Geometry, ShaderStage::Pixel, ShaderStage::Compute,
                                 ShaderStage::Task,     ShaderStage::Mesh};

  for(ShaderStage stage : stages)
  {
    rdcarray<UsedDescriptor> descs = pipe.GetReadOnlyResources(stage, false);
    for(const UsedDescriptor &ud : descs)
    {
      ResourceId rid = ud.descriptor.resource;
      if(rid != ResourceId() && !seen.contains(rid))
      {
        seen.append(rid);
        result.append(rid);
      }
    }
  }

  return result;
}

QList<ResourceId> BatchExporter::collectAllTextures()
{
  QList<ResourceId> result;
  rdcarray<TextureDescription> textures;

  m_Ctx.Replay().BlockInvoke(
      [&](IReplayController *r) { textures = r->GetTextures(); });

  for(const TextureDescription &td : textures)
  {
    if(td.resourceId != ResourceId())
      result.append(td.resourceId);
  }

  return result;
}

// ─────────────────────────── Texture export ─────────────────────────────────

int BatchExporter::exportTextures(const QList<ResourceId> &ids, int &skipped)
{
  int exported = 0;
  skipped = 0;

  for(ResourceId rid : ids)
  {
    QString name = sanitizeFileName(QString(m_Ctx.GetResourceName(rid)));
    if(name.isEmpty())
      name = QStringLiteral("unnamed");

    QString filename =
        QStringLiteral("%1/texture_%2_%3.png").arg(m_ExportDir, resourceIdHex(rid), name);

    TextureSave config;
    config.resourceId = rid;
    config.destType = FileType::PNG;
    config.mip = 0;
    config.slice.sliceIndex = 0;

    ResultDetails result;
    m_Ctx.Replay().BlockInvoke(
        [&](IReplayController *r) { result = r->SaveTexture(config, filename.toUtf8().data()); });

    if(result.OK())
      exported++;
    else
      skipped++;
  }

  return exported;
}

// ─────────────────────────── Mesh: find POSITION stream ─────────────────────

bool BatchExporter::isValidPositionFormat(const ResourceFormat &fmt)
{
  return fmt.type == ResourceFormatType::Regular &&
         (fmt.compType == CompType::Float || fmt.compType == CompType::SNorm ||
          fmt.compType == CompType::UNorm) &&
         fmt.compCount >= 3 && fmt.compByteWidth == 4;
}

void BatchExporter::fillFromAttr(PositionStream &out, const VertexInputAttribute &attr)
{
  out.vbSlot = attr.vertexBuffer;
  out.attrByteOffset = attr.byteOffset;
  out.compCount = attr.format.compCount;
  out.compByteWidth = attr.format.compByteWidth;
  out.valid = true;
}

BatchExporter::PositionStream BatchExporter::findPositionStream()
{
  PositionStream result;

  rdcarray<VertexInputAttribute> attrs = m_Ctx.CurPipelineState().GetVertexInputs();

  // Mirror BufferViewer::guessPositionColumn() - 4-level fallback

  // Pass 1: exact match POSITION / POSITION0 / POS / POS0
  for(const VertexInputAttribute &attr : attrs)
  {
    if(attr.perInstance || attr.genericEnabled)
      continue;
    QString n = QString(attr.name);
    if(n.compare(lit("POSITION"), Qt::CaseInsensitive) == 0 ||
       n.compare(lit("POSITION0"), Qt::CaseInsensitive) == 0 ||
       n.compare(lit("POS"), Qt::CaseInsensitive) == 0 ||
       n.compare(lit("POS0"), Qt::CaseInsensitive) == 0)
    {
      if(isValidPositionFormat(attr.format))
      {
        fillFromAttr(result, attr);
        return result;
      }
    }
  }

  // Pass 2: contains "POSITION"
  for(const VertexInputAttribute &attr : attrs)
  {
    if(attr.perInstance || attr.genericEnabled)
      continue;
    if(QString(attr.name).contains(lit("POSITION"), Qt::CaseInsensitive))
    {
      if(isValidPositionFormat(attr.format))
      {
        fillFromAttr(result, attr);
        return result;
      }
    }
  }

  // Pass 3: contains "POS"
  for(const VertexInputAttribute &attr : attrs)
  {
    if(attr.perInstance || attr.genericEnabled)
      continue;
    if(QString(attr.name).contains(lit("POS"), Qt::CaseInsensitive))
    {
      if(isValidPositionFormat(attr.format))
      {
        fillFromAttr(result, attr);
        return result;
      }
    }
  }

  // Pass 4: first non-instance float3/float4 attribute
  for(const VertexInputAttribute &attr : attrs)
  {
    if(attr.perInstance || attr.genericEnabled)
      continue;
    if(isValidPositionFormat(attr.format))
    {
      fillFromAttr(result, attr);
      return result;
    }
  }

  return result;
}

// ─────────────────────────── Read helper: float3/float4 from VB ─────────────

static QVector<QVector3D> readFloat3FromVB(const bytebuf &raw, uint32_t byteStride,
                                           uint32_t attrOffset, uint32_t numVerts)
{
  QVector<QVector3D> out;
  out.reserve((int)numVerts);

  for(uint32_t i = 0; i < numVerts; i++)
  {
    uint64_t base = (uint64_t)i * byteStride + attrOffset;
    if(base + 12 > raw.size())
      break;
    float x, y, z;
    memcpy(&x, raw.data() + base, 4);
    memcpy(&y, raw.data() + base + 4, 4);
    memcpy(&z, raw.data() + base + 8, 4);
    out.push_back(QVector3D(x, y, z));
  }

  return out;
}

// ─────────────────────────── Vertex positions ───────────────────────────────

QVector<QVector3D> BatchExporter::readVertexPositions(const PositionStream &pos,
                                                       const BoundVBuffer &vb,
                                                       uint32_t maxVerts,
                                                       IReplayController *r)
{
  if(!pos.valid || vb.resourceId == ResourceId() || vb.byteStride == 0)
    return {};

  bytebuf raw = r->GetBufferData(vb.resourceId, vb.byteOffset, 0);
  if(raw.empty())
    return {};

  uint32_t available = (uint32_t)(raw.size() / vb.byteStride);
  uint32_t numVerts = qMin(maxVerts, available);
  return readFloat3FromVB(raw, vb.byteStride, pos.attrByteOffset, numVerts);
}

// ─────────────────────────── Vertex normals (optional) ──────────────────────

QVector<QVector3D> BatchExporter::readVertexNormals(const PositionStream &posStream,
                                                     const BoundVBuffer &vb,
                                                     uint32_t maxVerts,
                                                     IReplayController *r)
{
  rdcarray<VertexInputAttribute> attrs = m_Ctx.CurPipelineState().GetVertexInputs();

  for(const VertexInputAttribute &attr : attrs)
  {
    QString semName = QString(attr.name).toUpper();
    if(semName != QStringLiteral("NORMAL") && semName != QStringLiteral("SV_NORMAL"))
      continue;
    if(attr.vertexBuffer != posStream.vbSlot || attr.perInstance || attr.genericEnabled)
      continue;
    const ResourceFormat &fmt = attr.format;
    if(fmt.type != ResourceFormatType::Regular || fmt.compByteWidth != 4 || fmt.compCount < 3)
      continue;

    bytebuf raw = r->GetBufferData(vb.resourceId, vb.byteOffset, 0);
    if(raw.empty())
      return {};
    uint32_t available = (uint32_t)(raw.size() / vb.byteStride);
    uint32_t numVerts = qMin(maxVerts, available);
    return readFloat3FromVB(raw, vb.byteStride, attr.byteOffset, numVerts);
  }

  return {};
}

// ─────────────────────────── Vertex UVs (optional) ──────────────────────────

QVector<QVector2D> BatchExporter::readVertexUVs(const PositionStream &posStream,
                                                 const BoundVBuffer &vb,
                                                 uint32_t maxVerts,
                                                 IReplayController *r)
{
  rdcarray<VertexInputAttribute> attrs = m_Ctx.CurPipelineState().GetVertexInputs();

  for(const VertexInputAttribute &attr : attrs)
  {
    QString semName = QString(attr.name).toUpper();
    if(!semName.startsWith(QStringLiteral("TEXCOORD")))
      continue;
    if(attr.vertexBuffer != posStream.vbSlot || attr.perInstance || attr.genericEnabled)
      continue;
    const ResourceFormat &fmt = attr.format;
    if(fmt.type != ResourceFormatType::Regular || fmt.compByteWidth != 4 || fmt.compCount < 2)
      continue;

    bytebuf raw = r->GetBufferData(vb.resourceId, vb.byteOffset, 0);
    if(raw.empty())
      return {};
    uint32_t available = (uint32_t)(raw.size() / vb.byteStride);
    uint32_t numVerts = qMin(maxVerts, available);

    QVector<QVector2D> uvs;
    uvs.reserve((int)numVerts);
    for(uint32_t i = 0; i < numVerts; i++)
    {
      uint64_t base = (uint64_t)i * vb.byteStride + attr.byteOffset;
      if(base + 8 > raw.size())
        break;
      float u, v;
      memcpy(&u, raw.data() + base, 4);
      memcpy(&v, raw.data() + base + 4, 4);
      uvs.push_back(QVector2D(u, v));
    }
    return uvs;
  }

  return {};
}

// ─────────────────────────── Index buffer ───────────────────────────────────

QVector<uint32_t> BatchExporter::readIndexBuffer(const BoundVBuffer &ib, uint32_t numIndices,
                                                  uint32_t baseVertex, IReplayController *r)
{
  if(ib.resourceId == ResourceId() || ib.byteStride == 0)
  {
    // Non-indexed draw: sequential indices for exactly numIndices vertices
    QVector<uint32_t> seq;
    seq.reserve((int)numIndices);
    for(uint32_t i = 0; i < numIndices; i++)
      seq.push_back(i);
    return seq;
  }

  bytebuf raw = r->GetBufferData(ib.resourceId, ib.byteOffset, 0);
  if(raw.empty())
    return {};

  uint32_t available = (uint32_t)(raw.size() / ib.byteStride);
  uint32_t count = qMin(numIndices, available);

  QVector<uint32_t> indices;
  indices.reserve((int)count);

  if(ib.byteStride == 2)
  {
    for(uint32_t i = 0; i < count; i++)
    {
      uint16_t idx;
      memcpy(&idx, raw.data() + i * 2, 2);
      indices.push_back((uint32_t)idx + baseVertex);
    }
  }
  else if(ib.byteStride == 4)
  {
    for(uint32_t i = 0; i < count; i++)
    {
      uint32_t idx;
      memcpy(&idx, raw.data() + i * 4, 4);
      indices.push_back(idx + baseVertex);
    }
  }

  return indices;
}

// ─────────────────────────── Write OBJ ──────────────────────────────────────

bool BatchExporter::writeObjFile(const QVector<QVector3D> &positions,
                                 const QVector<QVector3D> &normals,
                                 const QVector<QVector2D> &uvs,
                                 const QVector<uint32_t> &indices,
                                 ResourceId vbId,
                                 uint32_t eventId)
{
  if(positions.isEmpty())
    return false;

  QString name = sanitizeFileName(QString(m_Ctx.GetResourceName(vbId)));
  if(name.isEmpty())
    name = QStringLiteral("unnamed");

  QString filename =
      QStringLiteral("%1/mesh_%2_%3.obj").arg(m_ExportDir, resourceIdHex(vbId), name);

  QFile file(filename);
  if(!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate))
    return false;

  QTextStream ts(&file);
  ts << QStringLiteral("# RenderDoc Export - EventID %1\n").arg(eventId);
  ts << QStringLiteral("# Mesh: %1\n").arg(name);
  ts << QStringLiteral("# Vertices: %1\n").arg(positions.size());

  for(const QVector3D &v : positions)
    ts << QStringLiteral("v %1 %2 %3\n").arg(v.x()).arg(v.y()).arg(v.z());

  bool hasNormals = !normals.isEmpty() && normals.size() == positions.size();
  bool hasUVs = !uvs.isEmpty() && uvs.size() == positions.size();

  if(hasNormals)
    for(const QVector3D &n : normals)
      ts << QStringLiteral("vn %1 %2 %3\n").arg(n.x()).arg(n.y()).arg(n.z());

  if(hasUVs)
    for(const QVector2D &uv : uvs)
      ts << QStringLiteral("vt %1 %2\n").arg(uv.x()).arg(uv.y());

  // Write faces: each 3 consecutive indices form a triangle
  int triCount = indices.size() / 3;
  for(int t = 0; t < triCount; t++)
  {
    uint32_t i0 = indices[t * 3 + 0] + 1;    // OBJ is 1-based
    uint32_t i1 = indices[t * 3 + 1] + 1;
    uint32_t i2 = indices[t * 3 + 2] + 1;

    if(hasNormals && hasUVs)
      ts << QStringLiteral("f %1/%1/%1 %2/%2/%2 %3/%3/%3\n").arg(i0).arg(i1).arg(i2);
    else if(hasNormals)
      ts << QStringLiteral("f %1//%1 %2//%2 %3//%3\n").arg(i0).arg(i1).arg(i2);
    else if(hasUVs)
      ts << QStringLiteral("f %1/%1 %2/%2 %3/%3\n").arg(i0).arg(i1).arg(i2);
    else
      ts << QStringLiteral("f %1 %2 %3\n").arg(i0).arg(i1).arg(i2);
  }

  return true;
}

// ─────────────────────────── Export current DrawCall mesh ───────────────────

bool BatchExporter::exportCurrentDrawCallMesh(int &skipped)
{
  PositionStream pos = findPositionStream();
  if(!pos.valid)
  {
    skipped++;
    return false;
  }

  rdcarray<BoundVBuffer> vbs = m_Ctx.CurPipelineState().GetVBuffers();
  if(pos.vbSlot < 0 || pos.vbSlot >= (int)vbs.size())
  {
    skipped++;
    return false;
  }

  const BoundVBuffer &vb = vbs[pos.vbSlot];
  if(vb.resourceId == ResourceId() || vb.byteStride == 0)
  {
    skipped++;
    return false;
  }

  const ActionDescription *action = m_Ctx.CurSelectedAction();
  uint32_t eventId = action ? action->eventId : 0;
  uint32_t numIndices = action ? action->numIndices : 0;
  uint32_t baseVertex = action ? (uint32_t)action->baseVertex : 0;

  if(numIndices == 0)
  {
    skipped++;
    return false;
  }

  // Apply draw-call vertex/index offsets to match BufferViewer VSIn logic:
  //   vertexByteOffset = vb.byteOffset + action->vertexOffset * byteStride
  //   indexByteOffset  = ib.byteOffset + action->indexOffset  * byteStride
  BoundVBuffer adjustedVB = vb;
  if(action)
    adjustedVB.byteOffset = vb.byteOffset + (uint64_t)action->vertexOffset * vb.byteStride;

  BoundVBuffer adjustedIB = m_Ctx.CurPipelineState().GetIBuffer();
  if(action && adjustedIB.byteStride > 0)
    adjustedIB.byteOffset = adjustedIB.byteOffset +
                            (uint64_t)action->indexOffset * adjustedIB.byteStride;

  QVector<QVector3D> positions, normals;
  QVector<QVector2D> uvs;
  QVector<uint32_t> indices;

  m_Ctx.Replay().BlockInvoke([&](IReplayController *r) {
    // Read exactly numIndices entries (with baseVertex applied)
    indices = readIndexBuffer(adjustedIB, numIndices, baseVertex, r);

    // Determine the highest vertex index actually referenced so we only read those verts
    uint32_t maxVerts = 0;
    for(uint32_t idx : indices)
      if(idx + 1 > maxVerts)
        maxVerts = idx + 1;
    if(maxVerts == 0)
      maxVerts = numIndices;

    positions = readVertexPositions(pos, adjustedVB, maxVerts, r);
    normals = readVertexNormals(pos, adjustedVB, maxVerts, r);
    uvs = readVertexUVs(pos, adjustedVB, maxVerts, r);
  });

  if(positions.isEmpty())
  {
    skipped++;
    return false;
  }

  if(!writeObjFile(positions, normals, uvs, indices, vb.resourceId, eventId))
  {
    skipped++;
    return false;
  }

  return true;
}

// ─────────────────────────── Collect all drawcall event IDs ─────────────────

void BatchExporter::collectAllDrawcalls(const rdcarray<ActionDescription> &actions,
                                        QVector<uint32_t> &outEventIds)
{
  for(const ActionDescription &action : actions)
  {
    if(action.flags & ActionFlags::Drawcall)
      outEventIds.push_back(action.eventId);

    if(!action.children.empty())
      collectAllDrawcalls(action.children, outEventIds);
  }
}

// ─────────────────────────── Top-level export methods ───────────────────────

QString BatchExporter::exportDrawCallResources()
{
  if(!m_Ctx.IsCaptureLoaded() || !m_Ctx.CurSelectedAction())
    return QStringLiteral("No DrawCall selected.");

  initExportDir();

  int texSkipped = 0;
  QList<ResourceId> texIds = collectDrawCallTextures();
  int texExported = exportTextures(texIds, texSkipped);

  int meshSkipped = 0;
  exportCurrentDrawCallMesh(meshSkipped);
  int meshExported = (meshSkipped == 0) ? 1 : 0;

  QString msg = QStringLiteral("Exported %1 textures, %2 mesh to %3")
                    .arg(texExported)
                    .arg(meshExported)
                    .arg(m_ExportDir);
  if(texSkipped > 0 || meshSkipped > 0)
    msg += QStringLiteral(" (%1 skipped)").arg(texSkipped + meshSkipped);

  return msg;
}

QString BatchExporter::exportAllResources()
{
  if(!m_Ctx.IsCaptureLoaded())
    return QStringLiteral("No capture loaded.");

  initExportDir();

  // Export all textures
  int texSkipped = 0;
  QList<ResourceId> texIds = collectAllTextures();
  int texExported = exportTextures(texIds, texSkipped);

  // Export meshes from every drawcall, de-duplicating by VB ResourceId
  QList<ResourceId> exportedVBs;
  int meshExported = 0;
  int meshSkipped = 0;

  QVector<uint32_t> eventIds;
  collectAllDrawcalls(m_Ctx.CurRootActions(), eventIds);

  for(uint32_t eid : eventIds)
  {
    m_Ctx.Replay().BlockInvoke([&](IReplayController *r) { r->SetFrameEvent(eid, false); });

    PositionStream pos = findPositionStream();
    if(!pos.valid)
    {
      meshSkipped++;
      continue;
    }

    rdcarray<BoundVBuffer> vbs = m_Ctx.CurPipelineState().GetVBuffers();
    if(pos.vbSlot < 0 || pos.vbSlot >= (int)vbs.size())
    {
      meshSkipped++;
      continue;
    }

    ResourceId vbId = vbs[pos.vbSlot].resourceId;
    if(vbId == ResourceId() || exportedVBs.contains(vbId))
      continue;

    exportedVBs.append(vbId);

    int skippedThis = 0;
    if(exportCurrentDrawCallMesh(skippedThis))
      meshExported++;
    else
      meshSkipped++;
  }

  // Restore current event
  const ActionDescription *action = m_Ctx.CurSelectedAction();
  if(action)
  {
    uint32_t eid = action->eventId;
    m_Ctx.Replay().BlockInvoke([&](IReplayController *r) { r->SetFrameEvent(eid, false); });
  }

  QString msg = QStringLiteral("Exported %1 textures, %2 meshes to %3")
                    .arg(texExported)
                    .arg(meshExported)
                    .arg(m_ExportDir);
  if(texSkipped > 0 || meshSkipped > 0)
    msg += QStringLiteral(" (%1 skipped)").arg(texSkipped + meshSkipped);

  return msg;
}
