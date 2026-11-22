#include "render/gpu_model.h"

#include "base/log.h"
#include "render/deferred_uploads.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <type_traits>

namespace gt3r::render {

ModelStats model_stats(const scene::Model& model, int lod) {
    ModelStats s;
    s.textures = model.textures.size();
    for (const scene::Mesh& m : model.meshes) {
        s.max_lod = std::max(s.max_lod, m.lod);
        if (lod >= 0 && m.lod != lod) continue;
        ++s.meshes;
        s.vertices += m.vertices.size();
        s.primitives += m.primitives.size();
        for (const scene::Primitive& p : m.primitives) s.triangles += p.index_count / 3;
    }
    return s;
}

namespace {

// A mesh's sphere: its box centre, the farthest vertex.
void mesh_sphere(const std::vector<scene::Vertex>& vs, Vec3& center, float& radius) {
    if (vs.empty()) return;
    float lo[3], hi[3];
    for (int k = 0; k < 3; ++k) lo[k] = hi[k] = vs[0].pos[k];
    for (const scene::Vertex& v : vs)
        for (int k = 0; k < 3; ++k) lo[k] = std::min(lo[k], v.pos[k]), hi[k] = std::max(hi[k], v.pos[k]);
    center = {(lo[0] + hi[0]) * 0.5f, (lo[1] + hi[1]) * 0.5f, (lo[2] + hi[2]) * 0.5f};
    float r2 = 0;
    for (const scene::Vertex& v : vs) {
        Vec3 d = Vec3{v.pos[0], v.pos[1], v.pos[2]} - center;
        r2 = std::max(r2, dot(d, d));
    }
    radius = std::sqrt(r2);
}

// A primitive's sphere: the box centre of the vertices its `count` indices name, the farthest of them.
GpuModel::MeshInfo::Sphere primitive_sphere(const std::vector<scene::Vertex>& vs, const u32* idx, u32 count) {
    float lo[3], hi[3];
    for (int k = 0; k < 3; ++k) lo[k] = hi[k] = vs[idx[0]].pos[k];
    for (u32 i = 0; i < count; ++i)
        for (int k = 0; k < 3; ++k) lo[k] = std::min(lo[k], vs[idx[i]].pos[k]), hi[k] = std::max(hi[k], vs[idx[i]].pos[k]);
    GpuModel::MeshInfo::Sphere sp;
    sp.center = {(lo[0] + hi[0]) * 0.5f, (lo[1] + hi[1]) * 0.5f, (lo[2] + hi[2]) * 0.5f};
    float r2 = 0;
    for (u32 i = 0; i < count; ++i) {
        const float* v = vs[idx[i]].pos;
        const Vec3 d = Vec3{v[0], v[1], v[2]} - sp.center;
        r2 = std::max(r2, dot(d, d));
    }
    sp.radius = std::sqrt(r2);
    return sp;
}

// Meshes -> one vertex and one index list, per-mesh draw info; `lines`: wireframe edge lists.
void assemble(const std::vector<scene::Mesh>& meshes, size_t texture_count, bool lines,
              std::vector<scene::Vertex>& vertices, std::vector<u32>& indices, std::vector<GpuModel::MeshInfo>& out) {
    for (const scene::Mesh& m : meshes) {
        GpuModel::MeshInfo info;
        info.transform = Mat4::from(m.transform);
        info.lod = m.lod;
        info.node = m.node;
        info.vertex_offset = s32(vertices.size());
        info.index_offset = u32(indices.size());
        vertices.insert(vertices.end(), m.vertices.begin(), m.vertices.end());
        mesh_sphere(m.vertices, info.center, info.radius);
        u32 nv = u32(m.vertices.size());
        // Drop out-of-range indices instead of letting the GPU read garbage.
        for (const scene::Primitive& p : m.primitives) {
            scene::Primitive q = p;
            q.first_index = u32(indices.size()) - info.index_offset;
            u32 end = std::min<u32>(p.first_index + p.index_count, u32(m.indices.size()));
            u32 tri_end = p.first_index + (end > p.first_index ? (end - p.first_index) / 3 * 3 : 0);
            for (u32 i = p.first_index; i < tri_end; i += 3) {
                u32 a = m.indices[i], b = m.indices[i + 1], c = m.indices[i + 2];
                if (a >= nv || b >= nv || c >= nv) continue;
                indices.insert(indices.end(), {a, b, c});
            }
            q.index_count = u32(indices.size()) - info.index_offset - q.first_index;
            if (q.texture >= int(texture_count)) q.texture = -1;
            if (!q.index_count) continue;
            info.primitives.push_back(q);
            // Its sphere: the box centre, the farthest vertex.
            info.primitive_spheres.push_back(
                primitive_sphere(m.vertices, indices.data() + info.index_offset + q.first_index, q.index_count));
        }
        // Unique edges for the wireframe view.
        info.line_first = u32(indices.size()) - info.index_offset;
        if (lines) {
            // The mesh's edges, each once, in the order the triangles first name them. An open-addressing table
            // (an unordered_set allocated a node per edge: seconds for a course's scenery on a slow device).
            size_t tri_count = indices.size() - info.index_offset;
            int bits = 4;
            while ((size_t(1) << bits) < tri_count * 2 + 16) ++bits;
            std::vector<u64> table(size_t(1) << bits, ~u64(0)); // ~0: empty (no edge joins vertex 0xFFFFFFFF to itself)
            const size_t mask = table.size() - 1;
            for (size_t i = info.index_offset; i < info.index_offset + tri_count; i += 3)
                for (int e = 0; e < 3; ++e) {
                    u32 a = indices[i + e], b = indices[i + (e + 1) % 3];
                    if (a > b) std::swap(a, b);
                    const u64 key = (u64(a) << 32) | b;
                    size_t h = size_t((key * 0x9E3779B97F4A7C15ull) >> (64 - bits));
                    while (table[h] != ~u64(0) && table[h] != key) h = (h + 1) & mask;
                    if (table[h] == key) continue;
                    table[h] = key;
                    indices.insert(indices.end(), {a, b});
                }
        }
        info.line_count = u32(indices.size()) - info.index_offset - info.line_first;
        out.push_back(std::move(info));
    }
}

// Makes `buffer` hold at least `size` bytes, creating it or replacing it with one twice the size
// (contents lost). True when it is now a new buffer.
bool ensure_buffer(SDL_GPUDevice* device, SDL_GPUBufferUsageFlags usage, SDL_GPUBuffer*& buffer, u32& capacity,
                   u32 size, bool& ok) {
    ok = true;
    if (buffer && capacity >= size) return false;
    if (buffer) SDL_ReleaseGPUBuffer(device, buffer);
    SDL_GPUBufferCreateInfo bi{};
    bi.usage = usage;
    bi.size = std::max(size, capacity * 2);
    buffer = SDL_CreateGPUBuffer(device, &bi);
    capacity = buffer ? bi.size : 0;
    ok = buffer != nullptr;
    return true;
}

} // namespace

GpuModel::GpuModel(GpuContext& ctx, const scene::Model& model, bool keep_geometry)
    : ctx_(&ctx), device_(ctx.device()), keep_(keep_geometry) {
    std::vector<scene::Vertex> vertices;
    std::vector<u32> indices;
    assemble(model.meshes, model.textures.size(), true, vertices, indices, meshes_);
    index_nodes();
    vertex_count_ = vertices.size();
    for (const scene::Texture& t : model.textures) {
        TextureInfo ti;
        ti.clamp_u = t.clamp_u;
        ti.clamp_v = t.clamp_v;
        ti.no_mips = t.no_mips;
        textures_.push_back(ti);
    }
    if (DeferredUploads* d = DeferredUploads::current()) { // (made on a worker thread: upload later, on the GPU's)
        if (keep_) geometry_ = {vertices, indices};
        d->add([this, v = std::move(vertices), i = std::move(indices), t = model.textures] { upload(v, i, t); });
        return;
    }
    upload(vertices, indices, model.textures);
    if (keep_) geometry_ = {std::move(vertices), std::move(indices)};
}

void GpuModel::upload(const std::vector<scene::Vertex>& vertices, const std::vector<u32>& indices,
                      const std::vector<scene::Texture>& textures) {
    Uploader up(device_);
    if (!vertices.empty() && !indices.empty()) {
        vertices_ = up.buffer(SDL_GPU_BUFFERUSAGE_VERTEX, vertices.data(), u32(vertices.size() * sizeof(scene::Vertex)));
        indices_ = up.buffer(SDL_GPU_BUFFERUSAGE_INDEX, indices.data(), u32(indices.size() * sizeof(u32)));
    }
    for (size_t k = 0; k < textures.size() && k < textures_.size(); ++k) {
        const scene::Texture& t = textures[k];
        TextureInfo& ti = textures_[k];
        if (t.width && t.height && t.rgba.size() >= size_t(t.width) * t.height) {
            ti.texture = ctx_->acquire_texture(up, t.width, t.height, t.rgba.data());
            ti.width = t.width, ti.height = t.height;
        }
        if (!ti.texture) LOG_WARN("Texture '%s' (%ux%u) not uploaded", t.name.c_str(), t.width, t.height);
    }
    if (!up.submit()) LOG_WARN("Model upload failed: %s", SDL_GetError());
}

// Each upload goes through staging_ (grown when too small), at 16-byte aligned offsets, into its
// buffer from offset 0 with cycling: frames still drawing keep the old contents.
bool GpuModel::stage(const Upload* uploads, int count) {
    auto aligned = [](u32 v) { return (v + 15u) & ~15u; };
    u32 total = 0;
    for (int i = 0; i < count; ++i) total += aligned(uploads[i].size);
    if (total == 0) return true;
    if (!staging_ || staging_capacity_ < total) {
        if (staging_) SDL_ReleaseGPUTransferBuffer(device_, staging_);
        SDL_GPUTransferBufferCreateInfo ti{};
        ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        ti.size = std::max(total, staging_capacity_ * 2);
        staging_ = SDL_CreateGPUTransferBuffer(device_, &ti);
        staging_capacity_ = staging_ ? ti.size : 0;
        if (!staging_) return false;
    }
    auto* p = static_cast<u8*>(SDL_MapGPUTransferBuffer(device_, staging_, true));
    if (!p) return false;
    for (int i = 0, at = 0; i < count; at += int(aligned(uploads[i].size)), ++i)
        std::memcpy(p + at, uploads[i].data, uploads[i].size);
    SDL_UnmapGPUTransferBuffer(device_, staging_);
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    if (!cmd) return false;
    SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
    for (int i = 0, at = 0; i < count; at += int(aligned(uploads[i].size)), ++i) {
        if (!uploads[i].size) continue;
        SDL_GPUTransferBufferLocation src{staging_, u32(at)};
        SDL_GPUBufferRegion dst{uploads[i].buffer, 0, uploads[i].size};
        SDL_UploadToGPUBuffer(cp, &src, &dst, true);
    }
    SDL_EndGPUCopyPass(cp);
    if (!SDL_SubmitGPUCommandBuffer(cmd)) return false;
    note_gpu_submits(device_, 1); // staging_ is this model's own: no new transfer buffer
    return true;
}

bool GpuModel::update_vertices(const std::vector<scene::Vertex>& all) {
    if (!vertices_ || all.size() != vertex_count_) return false;
    const Upload u{vertices_, all.data(), u32(all.size() * sizeof(scene::Vertex))};
    if (keep_) geometry_.vertices = all;
    return stage(&u, 1);
}

bool GpuModel::update_textures(const std::vector<TextureUpdate>& updates) {
    bool ok = true;
    Uploader up(device_);
    for (const TextureUpdate& u : updates) {
        TextureInfo* t = u.index < textures_.size() ? &textures_[u.index] : nullptr;
        if (!t || !t->texture || !u.rgba || u.rgba->size() != size_t(t->width) * t->height) {
            ok = false;
            continue;
        }
        if (ctx_->take_texture(t->texture)) {
            up.update_rgba(t->texture, t->width, t->height, u.rgba->data(), true, true);
        } else if (SDL_GPUTexture* own = up.texture_rgba(t->width, t->height, u.rgba->data(), true)) {
            ctx_->release_texture(t->texture); // other models keep drawing the shared texels
            t->texture = own;
        } else {
            ok = false;
        }
    }
    return up.submit() && ok;
}

GpuModel::GpuModel(GpuContext& ctx, const GpuModel& textures)
    : ctx_(&ctx), device_(ctx.device()), texture_source_(&textures) {}

void GpuModel::assemble_geometry(const std::vector<scene::Mesh>& meshes, size_t texture_count,
                                 std::vector<scene::Vertex>& vertices, std::vector<u32>& indices,
                                 std::vector<MeshInfo>& infos) {
    assemble(meshes, texture_count, false, vertices, indices, infos);
}

std::vector<GpuModel::TriangleLayout> GpuModel::triangle_layout(const std::vector<scene::Mesh>& meshes) {
    std::vector<TriangleLayout> out;
    out.reserve(meshes.size());
    for (const scene::Mesh& m : meshes) out.push_back({m.vertices.size(), m.node, m.lod, m.transform, m.indices, m.primitives});
    return out;
}

bool GpuModel::same_triangles(const std::vector<TriangleLayout>& last, const std::vector<scene::Mesh>& meshes) {
    static_assert(std::is_trivially_copyable_v<scene::Primitive>, "compared as bytes");
    if (meshes.size() != last.size()) return false;
    for (size_t i = 0; i < meshes.size(); ++i) {
        const scene::Mesh& m = meshes[i];
        const TriangleLayout& l = last[i];
        if (m.vertices.size() != l.vertex_count || m.node != l.node || m.lod != l.lod || m.transform != l.transform ||
            m.indices != l.indices || m.primitives.size() != l.primitives.size() ||
            (!m.primitives.empty() &&
             std::memcmp(m.primitives.data(), l.primitives.data(), m.primitives.size() * sizeof(scene::Primitive)) != 0))
            return false;
    }
    return true;
}

void GpuModel::place_vertices(const std::vector<scene::Mesh>& meshes, const std::vector<u32>& indices,
                              std::vector<MeshInfo>& infos, std::vector<scene::Vertex>& vertices) {
    for (size_t i = 0; i < meshes.size(); ++i) {
        const scene::Mesh& m = meshes[i];
        MeshInfo& info = infos[i];
        if (!m.vertices.empty())
            std::memcpy(vertices.data() + info.vertex_offset, m.vertices.data(), m.vertices.size() * sizeof(scene::Vertex));
        mesh_sphere(m.vertices, info.center, info.radius);
        for (size_t j = 0; j < info.primitives.size(); ++j) {
            const scene::Primitive& q = info.primitives[j];
            info.primitive_spheres[j] = primitive_sphere(m.vertices, indices.data() + info.index_offset + q.first_index, q.index_count);
        }
    }
}

bool GpuModel::update_geometry(const std::vector<scene::Mesh>& meshes) {
    // The same triangles as the last upload (an animated node that only moved or morphed): only the vertices
    // change. Each mesh's vertices go where assemble put them, and the spheres are worked out as assemble does
    // (mesh_sphere, primitive_sphere), from the index buffer it made; that buffer is not sent again.
    if (vertices_ && !uploaded_indices_.empty() && meshes.size() == meshes_.size() && same_triangles(last_, meshes)) {
        std::vector<scene::Vertex>& vertices = scratch_vertices_;
        vertices.resize(vertex_count_);
        place_vertices(meshes, uploaded_indices_, meshes_, vertices);
        const u32 vsize = u32(vertices.size() * sizeof(scene::Vertex));
        if (vsize == 0) return true;
        Upload up{vertices_, vertices.data(), vsize};
        const bool ok = stage(&up, 1);
        if (ok && keep_) geometry_.vertices.assign(vertices.begin(), vertices.end());
        if (!ok) meshes_.clear(), node_start_.clear(), node_meshes_.clear(), uploaded_indices_.clear(), last_.clear(), geometry_ = {};
        return ok;
    }
    std::vector<scene::Vertex> vertices;
    std::vector<u32> indices;
    meshes_.clear();
    last_.clear();
    assemble_geometry(meshes, textures().size(), vertices, indices, meshes_);
    index_nodes();
    vertex_count_ = vertices.size();
    const u32 vsize = u32(vertices.size() * sizeof(scene::Vertex)), isize = u32(indices.size() * sizeof(u32));
    if (vsize == 0 || isize == 0) return true; // nothing drawn (meshes_ is empty too)
    bool vok = true, iok = true;
    ensure_buffer(device_, SDL_GPU_BUFFERUSAGE_VERTEX, vertices_, vertex_capacity_, vsize, vok);
    // The index buffer only when its contents change (the animated nodes' usually don't).
    if (ensure_buffer(device_, SDL_GPU_BUFFERUSAGE_INDEX, indices_, index_capacity_, isize, iok))
        uploaded_indices_.clear();
    Upload uploads[2] = {{vertices_, vertices.data(), vsize}, {indices_, indices.data(), isize}};
    const bool same_indices = indices == uploaded_indices_;
    bool ok = vok && iok && stage(uploads, same_indices ? 1 : 2);
    if (ok && keep_) geometry_ = {std::move(vertices), indices};
    if (ok && !same_indices) uploaded_indices_ = std::move(indices);
    if (!ok) meshes_.clear(), node_start_.clear(), node_meshes_.clear(), uploaded_indices_.clear(), geometry_ = {};
    if (ok) last_ = triangle_layout(meshes); // (what the fast path compares the next meshes with)
    return ok;
}

void GpuModel::index_nodes() {
    node_start_.clear(), node_meshes_.clear();
    int top = -1;
    for (const MeshInfo& m : meshes_) top = std::max(top, m.node);
    if (top < 0) return;
    // A counting sort: the meshes of each node, in model order.
    node_start_.assign(size_t(top) + 2, 0);
    for (const MeshInfo& m : meshes_)
        if (m.node >= 0) ++node_start_[size_t(m.node) + 1];
    for (size_t n = 1; n < node_start_.size(); ++n) node_start_[n] += node_start_[n - 1];
    node_meshes_.resize(node_start_.back());
    std::vector<u32> next(node_start_.begin(), node_start_.end() - 1);
    for (size_t i = 0; i < meshes_.size(); ++i)
        if (meshes_[i].node >= 0) node_meshes_[next[size_t(meshes_[i].node)]++] = u32(i);
}

GpuModel::~GpuModel() {
    if (staging_) SDL_ReleaseGPUTransferBuffer(device_, staging_);
    if (vertices_) SDL_ReleaseGPUBuffer(device_, vertices_);
    if (indices_) SDL_ReleaseGPUBuffer(device_, indices_);
    for (TextureInfo& t : textures_) ctx_->release_texture(t.texture);
}

} // namespace gt3r::render
