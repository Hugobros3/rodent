#include <iostream>
#include <fstream>
#include <chrono>
#include <numeric>
#include <algorithm>
#include <string>
#include <cstring>

#include <embree2/rtcore.h>
#include <embree2/rtcore_ray.h>
#include <common/math/vec3.h>
#include <common/math/vec3fa.h>

#include <anydsl_runtime.hpp>

#include "traversal.h"
#include "load_obj.h"
#include "load_rays.h"
#include "tri.h"

template <>
struct RayTraits<RTCRay8> {
    enum { RayPerPacket = 8 };
    static void write_ray(const float* org_dir, float tmin, float tmax, int j, RTCRay8& ray) {
        ray.orgx[j] = org_dir[0];
        ray.orgy[j] = org_dir[1];
        ray.orgz[j] = org_dir[2];
        ray.tnear[j] = tmin;
        ray.dirx[j] = org_dir[3];
        ray.diry[j] = org_dir[4];
        ray.dirz[j] = org_dir[5];
        ray.tfar[j] = tmax;
        ray.geomID[j] = RTC_INVALID_GEOMETRY_ID;
        ray.primID[j] = RTC_INVALID_GEOMETRY_ID;
        ray.instID[j] = RTC_INVALID_GEOMETRY_ID;
        ray.mask[j] = 0xFFFFFFFF;
        ray.time[j] = 0.0f;
    }
};

    
inline void check_argument(int i, int argc, char** argv) {
    if (i + 1 >= argc) {
        std::cerr << "Missing argument for " << argv[i] << std::endl;
        exit(1);
    }
}

inline void usage() {
    std::cout << "Usage: bench_embree [options]\n"
                 "Available options:\n"
                 "  -obj     --obj-file        Sets the OBJ file to use\n"
                 "  -ray     --ray-file        Sets the ray file to use\n"
                 "  -tmin                      Sets the minimum distance along the rays (default: 0)\n"
                 "  -tmax                      Sets the maximum distance along the rays (default: 1e9)\n"
                 "  -bench   --bench-iters     Sets the number of benchmark iterations (default: 1)\n"
                 "  -warmup  --bench-warmup    Sets the number of warmup iterations (default: 0)\n"
                 "  -any                       Exit at the first intersection (disabled by default)\n"
                 "  -o       --output          Sets the output file name (no file is generated by default)\n";
}

static void create_triangles(const obj::File& obj_file, std::vector<Tri>& tris) {
    for (auto& object : obj_file.objects) {
        for (auto& group : object.groups) {
            for (auto& face : group.faces) {
                auto v0 = obj_file.vertices[face.indices[0].v];
                for (int i = 0; i < face.index_count - 2; i++) {
                    auto v1 = obj_file.vertices[face.indices[i + 1].v];
                    auto v2 = obj_file.vertices[face.indices[i + 2].v];
                    tris.emplace_back(float3(v0.x, v0.y, v0.z),
                                      float3(v1.x, v1.y, v1.z),
                                      float3(v2.x, v2.y, v2.z));
                }
            }
        }
    }
}

#include <unistd.h>

template <typename F>
double intersect_scene(RTCScene scene,
                       const anydsl::Array<RTCRay8>& rays,
                       anydsl::Array<RTCRay8>& hits,
                       F f) {
    using namespace std::chrono;

    // Restore tnear, tfar and other fields that have been modified
    anydsl::copy(rays, hits);

    auto t0 = high_resolution_clock::now();
    for (int i = 0; i < rays.size(); i++) {
        const int valid[8] alignas(32) = { -1, -1, -1, -1, -1, -1, -1, -1 };
        f(valid, scene, hits[i]);
    }
    auto t1 = high_resolution_clock::now();

    return duration_cast<microseconds>(t1 - t0).count() * 1.0e-3;
}

void error_handler(const RTCError code, const char* str) {
    if (code == RTC_NO_ERROR)
        return;

    std::cerr << "Embree error: ";
    switch (code) {
        case RTC_UNKNOWN_ERROR:     std::cerr << "RTC_UNKNOWN_ERROR";       break;
        case RTC_INVALID_ARGUMENT:  std::cerr << "RTC_INVALID_ARGUMENT";    break;
        case RTC_INVALID_OPERATION: std::cerr << "RTC_INVALID_OPERATION";   break;
        case RTC_OUT_OF_MEMORY:     std::cerr << "RTC_OUT_OF_MEMORY";       break;
        case RTC_UNSUPPORTED_CPU:   std::cerr << "RTC_UNSUPPORTED_CPU";     break;
        case RTC_CANCELLED:         std::cerr << "RTC_CANCELLED";           break;
        default:                    std::cerr << "invalid error code";      break;
    }

    if (str) std::cerr << " (" << str << ")";
    std::cerr << std::endl;

    exit(1);
}

void bench(const std::vector<Tri>& tris,
           const anydsl::Array<RTCRay8>& rays,
           anydsl::Array<RTCRay8>& hits,
           bool any_hit, int iters, int warmup) {
    using namespace embree;

    auto device = rtcNewDevice("tri_accel=bvh4.triangle4");
    error_handler(rtcDeviceGetError(device), "");
    rtcDeviceSetErrorFunction(device, error_handler);

    auto scene = rtcDeviceNewScene(device, RTC_SCENE_STATIC, RTC_INTERSECT8);
    auto mesh = rtcNewTriangleMesh(scene, RTC_GEOMETRY_STATIC, tris.size(), tris.size() * 3);

    auto vertices = (Vec3fa*)rtcMapBuffer(scene, mesh, RTC_VERTEX_BUFFER); 
    for (int i = 0; i < tris.size(); i++) {
        vertices[3 * i + 0] = Vec3fa(tris[i].v0.x, tris[i].v0.y, tris[i].v0.z);
        vertices[3 * i + 1] = Vec3fa(tris[i].v1.x, tris[i].v1.y, tris[i].v1.z);
        vertices[3 * i + 2] = Vec3fa(tris[i].v2.x, tris[i].v2.y, tris[i].v2.z);
    }
    rtcUnmapBuffer(scene, mesh, RTC_VERTEX_BUFFER); 

    auto triangles = (int*)rtcMapBuffer(scene, mesh, RTC_INDEX_BUFFER);
    for (int i = 0; i < tris.size(); i++) {
        triangles[i * 3 + 0] = i * 3 + 0;
        triangles[i * 3 + 1] = i * 3 + 1;
        triangles[i * 3 + 2] = i * 3 + 2;
    }
    rtcUnmapBuffer(scene, mesh, RTC_INDEX_BUFFER);
    rtcCommit(scene);

    std::vector<double> timings(iters);
    if (any_hit) {
        for (int i = 0; i < warmup; i++) intersect_scene(scene, rays, hits, rtcOccluded8);
        for (int i = 0; i < iters ; i++) timings[i] = intersect_scene(scene, rays, hits, rtcOccluded8);
    } else {
        for (int i = 0; i < warmup; i++) intersect_scene(scene, rays, hits, rtcIntersect8);
        for (int i = 0; i < iters ; i++) timings[i] = intersect_scene(scene, rays, hits, rtcIntersect8);
    }

    size_t intr = 0;
    for (auto& hit : hits) {
        for (int i = 0; i < 8; i++) intr += hit.primID[i] != RTC_INVALID_GEOMETRY_ID;
    }

    std::sort(timings.begin(), timings.end());
    auto sum = std::accumulate(timings.begin(), timings.end(), 0.0);
    auto avg = sum / timings.size();
    auto med = timings[timings.size() / 2];
    auto min = *std::min_element(timings.begin(), timings.end());
    std::cout << sum << "ms for " << iters << " iteration(s)" << std::endl;
    std::cout << rays.size() * 8 * iters / (1000.0 * sum) << " Mrays/sec" << std::endl;
    std::cout << "# Average: " << avg << " ms" << std::endl;
    std::cout << "# Median: " << med  << " ms" << std::endl;
    std::cout << "# Min: " << min << " ms" << std::endl;
    std::cout << intr << " intersection(s)" << std::endl;

    rtcDeleteScene(scene);
    rtcDeleteDevice(device);
}

int main(int argc, char** argv) {
    std::string ray_file;
    std::string obj_file;
    std::string out_file;
    float tmin = 0.0f, tmax = 1e9f;
    int iters = 1;
    int warmup = 0;
    bool use_gpu = false;
    bool any_hit = false;

    for (int i = 1; i < argc; i++) {
        auto arg = argv[i];
        if (arg[0] == '-') {
            if (!strcmp(arg, "-h") || !strcmp(arg, "--help")) {
                usage();
                return 0;
            } else if (!strcmp(arg, "-obj") || !strcmp(arg, "--obj-file")) {
                check_argument(i, argc, argv);
                obj_file = argv[++i];
            } else if (!strcmp(arg, "-ray") || !strcmp(arg, "--ray-file")) {
                check_argument(i, argc, argv);
                ray_file = argv[++i];
            } else if (!strcmp(arg, "-tmin")) {
                check_argument(i, argc, argv);
                tmin = strtof(argv[++i], nullptr);
            } else if (!strcmp(arg, "-tmax")) {
                check_argument(i, argc, argv);
                tmax = strtof(argv[++i], nullptr);
            } else if (!strcmp(arg, "-bench") || !strcmp(arg, "--bench-iters")) {
                check_argument(i, argc, argv);
                iters = strtol(argv[++i], nullptr, 10);
            } else if (!strcmp(arg, "-warmup") || !strcmp(arg, "--warmup-iters")) {
                check_argument(i, argc, argv);
                warmup = strtol(argv[++i], nullptr, 10);
            } else if (!strcmp(arg, "-any")) {
                any_hit = true;
            } else if (!strcmp(arg, "-o") || !strcmp(arg, "--output")) {
                check_argument(i, argc, argv);
                out_file = argv[++i];
            } else {
                std::cerr << "Unknown option '" << arg << "'" << std::endl;
                return 1;
            }
        } else {
            std::cerr << "Invalid argument '" << arg << "'" << std::endl;
            return 1;
        }
    }

    if (obj_file == "") {
        std::cerr << "No OBJ file specified" << std::endl;
        return 1;
    }
    if (ray_file == "") {
        std::cerr << "No ray file specified" << std::endl;
        return 1;
    }

    obj::File obj;
    if (!load_obj(obj_file, obj)) {
        std::cerr << "Cannot load OBJ file" << std::endl;
        return 1;
    }

    std::vector<Tri> tris;
    create_triangles(obj, tris);

    anydsl::Array<RTCRay8> rays;
    if (!load_rays(ray_file, rays, tmin, tmax, false)) {
        std::cerr << "Cannot load rays" << std::endl;
        return 1;
    }

    std::cout << rays.size() * 8 << " ray(s) in the distribution file." << std::endl;

    anydsl::Array<RTCRay8> hits(rays.size());
    bench(tris, rays, hits, any_hit, iters, warmup);

    if (out_file != "") {
        std::ofstream of(out_file, std::ofstream::binary);
        for (auto& hit : hits) {
            for (int i = 0; i < 8; i++)
                of.write((char*)&hit.tfar[i], sizeof(float));
        }
    }

    return 0;
}
