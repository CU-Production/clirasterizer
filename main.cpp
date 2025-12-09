#include <iostream>
#include <iomanip>
#include <vector>
#include <array>
#include <cmath>
#include <algorithm>
#include <string>
#include <limits>
#include <chrono>
#include <cstring>

#ifdef _WIN32
#define NOMINMAX  // Prevent windows.h from defining min/max macros
#include <conio.h>
#include <windows.h>
#else
#include <unistd.h>
#include <termios.h>
#include <fcntl.h>
#endif

#define TINYOBJ_LOADER_C_IMPLEMENTATION
#include "tinyobj_loader_c.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "HandmadeMath.h"
#include "parallel-util.hpp"

// ============================================================================
// Platform-specific keyboard input
// ============================================================================

#ifdef _WIN32
// Windows: use _kbhit() and _getch() from conio.h
inline bool keyboard_hit() { return _kbhit() != 0; }
inline int get_char() { return _getch(); }

// Windows: Get terminal window size (columns, rows)
inline void get_terminal_size(int& width, int& height) {
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (GetConsoleScreenBufferInfo(hOut, &csbi)) {
        width = csbi.srWindow.Right - csbi.srWindow.Left + 1;
        height = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
    } else {
        width = 120;   // Default fallback
        height = 30;
    }
}
#else
// Unix/Linux: implement non-blocking keyboard input
inline bool keyboard_hit() {
    struct termios oldt, newt;
    int ch;
    int oldf;
    
    tcgetattr(STDIN_FILENO, &oldt);
    newt = oldt;
    newt.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);
    oldf = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, oldf | O_NONBLOCK);
    
    ch = getchar();
    
    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    fcntl(STDIN_FILENO, F_SETFL, oldf);
    
    if (ch != EOF) {
        ungetc(ch, stdin);
        return true;
    }
    return false;
}
inline int get_char() { return getchar(); }

// Unix/Linux: Get terminal window size (columns, rows)
#include <sys/ioctl.h>
inline void get_terminal_size(int& width, int& height) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        width = ws.ws_col;
        height = ws.ws_row;
    } else {
        width = 120;   // Default fallback
        height = 30;
    }
}
#endif

// ============================================================================
// Configuration
// ============================================================================

// Default fallback resolution
constexpr int DEFAULT_WIDTH = 120;
constexpr int DEFAULT_HEIGHT = 30;

// Reserve rows for status display at bottom
constexpr int STATUS_ROWS = 3;

// ============================================================================
// Color structure
// ============================================================================

struct Color {
    uint8_t r, g, b;
    
    Color() : r(0), g(0), b(0) {}
    Color(uint8_t r, uint8_t g, uint8_t b) : r(r), g(g), b(b) {}
    
    Color operator*(float f) const {
        return Color(
            static_cast<uint8_t>(std::clamp(r * f, 0.0f, 255.0f)),
            static_cast<uint8_t>(std::clamp(g * f, 0.0f, 255.0f)),
            static_cast<uint8_t>(std::clamp(b * f, 0.0f, 255.0f))
        );
    }
    
    Color operator+(const Color& other) const {
        return Color(
            static_cast<uint8_t>(std::clamp(r + other.r, 0, 255)),
            static_cast<uint8_t>(std::clamp(g + other.g, 0, 255)),
            static_cast<uint8_t>(std::clamp(b + other.b, 0, 255))
        );
    }
};

// ============================================================================
// Framebuffer - stores color and depth for each pixel
// ============================================================================

class Framebuffer {
public:
    int width, height;
    std::vector<Color> color_buffer;
    std::vector<float> depth_buffer;
    
    Framebuffer(int w, int h) : width(w), height(h) {
        color_buffer.resize(w * h);
        depth_buffer.resize(w * h);
        clear();
    }
    
    void clear() {
        Color bg_color(20, 20, 30);
        float max_depth = std::numeric_limits<float>::max();
        
        // Parallel clear for better performance
        parallelutil::parallel_for(width * height, [&](int i) {
            color_buffer[i] = bg_color;
            depth_buffer[i] = max_depth;
        });
    }
    
    // Simple pixel write with depth test (not thread-safe, use within single tile)
    void set_pixel(int x, int y, const Color& color, float depth) {
        if (x < 0 || x >= width || y < 0 || y >= height) return;
        int idx = y * width + x;
        if (depth < depth_buffer[idx]) {
            depth_buffer[idx] = depth;
            color_buffer[idx] = color;
        }
    }
    
    Color get_pixel(int x, int y) const {
        if (x < 0 || x >= width || y < 0 || y >= height) return Color();
        return color_buffer[y * width + x];
    }
    
    // Save framebuffer to PNG file for debugging
    bool save_to_file(const char* filename) const {
        std::vector<uint8_t> pixels(width * height * 3);
        for (int i = 0; i < width * height; i++) {
            pixels[i * 3 + 0] = color_buffer[i].r;
            pixels[i * 3 + 1] = color_buffer[i].g;
            pixels[i * 3 + 2] = color_buffer[i].b;
        }
        int result = stbi_write_png(filename, width, height, 3, pixels.data(), width * 3);
        return result != 0;
    }
    
    // Resize framebuffer to new dimensions
    void resize(int new_width, int new_height) {
        if (new_width == width && new_height == height) return;
        width = new_width;
        height = new_height;
        color_buffer.resize(width * height);
        depth_buffer.resize(width * height);
        clear();
    }
};

// ============================================================================
// Texture - loads and samples image textures
// ============================================================================

class Texture {
public:
    int width = 0, height = 0, channels = 0;
    std::vector<uint8_t> data;
    bool loaded = false;
    
    bool load(const char* filename) {
        uint8_t* img_data = stbi_load(filename, &width, &height, &channels, 3);
        if (!img_data) {
            std::cerr << "Failed to load texture: " << filename << std::endl;
            return false;
        }
        data.assign(img_data, img_data + width * height * 3);
        stbi_image_free(img_data);
        loaded = true;
        return true;
    }
    
    Color sample(float u, float v) const {
        if (!loaded) return Color(200, 200, 200);
        
        // Wrap UV coordinates
        u = u - std::floor(u);
        v = v - std::floor(v);
        
        int x = static_cast<int>(u * (width - 1));
        int y = static_cast<int>((1.0f - v) * (height - 1));  // Flip V
        
        x = std::clamp(x, 0, width - 1);
        y = std::clamp(y, 0, height - 1);
        
        int idx = (y * width + x) * 3;
        return Color(data[idx], data[idx + 1], data[idx + 2]);
    }
};

// ============================================================================
// Vertex structure for rendering
// ============================================================================

struct Vertex {
    HMM_Vec3 position;
    HMM_Vec2 texcoord;
    HMM_Vec3 normal;
};

// ============================================================================
// Mesh - stores geometry data
// ============================================================================

class Mesh {
public:
    std::vector<Vertex> vertices;
    std::vector<unsigned int> indices;
    
    bool load_obj(const char* filename) {
        tinyobj_attrib_t attrib;
        tinyobj_shape_t* shapes = nullptr;
        size_t num_shapes = 0;
        tinyobj_material_t* materials = nullptr;
        size_t num_materials = 0;
        
        // File reading callback
        auto file_reader = [](void* ctx, const char* filename, int is_mtl,
                              const char* obj_filename, char** buf, size_t* len) {
            (void)ctx;
            (void)is_mtl;
            (void)obj_filename;
            
            FILE* f = fopen(filename, "rb");
            if (!f) {
                *buf = nullptr;
                *len = 0;
                return;
            }
            
            fseek(f, 0, SEEK_END);
            *len = ftell(f);
            fseek(f, 0, SEEK_SET);
            
            *buf = (char*)malloc(*len + 1);
            fread(*buf, 1, *len, f);
            (*buf)[*len] = '\0';
            fclose(f);
        };
        
        int result = tinyobj_parse_obj(&attrib, &shapes, &num_shapes,
                                       &materials, &num_materials,
                                       filename, file_reader, nullptr,
                                       TINYOBJ_FLAG_TRIANGULATE);
        
        if (result != TINYOBJ_SUCCESS) {
            std::cerr << "Failed to load OBJ: " << filename << std::endl;
            return false;
        }
        
        // Convert to our vertex format
        vertices.clear();
        indices.clear();
        
        for (size_t i = 0; i < attrib.num_faces; i++) {
            tinyobj_vertex_index_t idx = attrib.faces[i];
            
            Vertex v;
            
            // Position
            v.position.X = attrib.vertices[3 * idx.v_idx + 0];
            v.position.Y = attrib.vertices[3 * idx.v_idx + 1];
            v.position.Z = attrib.vertices[3 * idx.v_idx + 2];
            
            // Texcoord
            if (idx.vt_idx >= 0 && attrib.num_texcoords > 0) {
                v.texcoord.X = attrib.texcoords[2 * idx.vt_idx + 0];
                v.texcoord.Y = attrib.texcoords[2 * idx.vt_idx + 1];
            } else {
                v.texcoord = HMM_V2(0, 0);
            }
            
            // Normal
            if (idx.vn_idx >= 0 && attrib.num_normals > 0) {
                v.normal.X = attrib.normals[3 * idx.vn_idx + 0];
                v.normal.Y = attrib.normals[3 * idx.vn_idx + 1];
                v.normal.Z = attrib.normals[3 * idx.vn_idx + 2];
            } else {
                v.normal = HMM_V3(0, 1, 0);
            }
            
            vertices.push_back(v);
            indices.push_back(static_cast<unsigned int>(vertices.size() - 1));
        }
        
        // Clean up
        tinyobj_attrib_free(&attrib);
        tinyobj_shapes_free(shapes, num_shapes);
        tinyobj_materials_free(materials, num_materials);
        
        std::cout << "Loaded mesh with " << vertices.size() << " vertices" << std::endl;
        return true;
    }
    
    // Calculate bounding box and return center and scale
    void get_bounds(HMM_Vec3& center, float& scale) const {
        HMM_Vec3 min_bound = HMM_V3(std::numeric_limits<float>::max(),
                                     std::numeric_limits<float>::max(),
                                     std::numeric_limits<float>::max());
        HMM_Vec3 max_bound = HMM_V3(std::numeric_limits<float>::lowest(),
                                     std::numeric_limits<float>::lowest(),
                                     std::numeric_limits<float>::lowest());
        
        for (const auto& v : vertices) {
            min_bound.X = std::min(min_bound.X, v.position.X);
            min_bound.Y = std::min(min_bound.Y, v.position.Y);
            min_bound.Z = std::min(min_bound.Z, v.position.Z);
            max_bound.X = std::max(max_bound.X, v.position.X);
            max_bound.Y = std::max(max_bound.Y, v.position.Y);
            max_bound.Z = std::max(max_bound.Z, v.position.Z);
        }
        
        center = HMM_MulV3F(HMM_AddV3(min_bound, max_bound), 0.5f);
        float dx = max_bound.X - min_bound.X;
        float dy = max_bound.Y - min_bound.Y;
        float dz = max_bound.Z - min_bound.Z;
        scale = std::max({dx, dy, dz});
    }
};

// ============================================================================
// Rasterizer - software triangle rasterization with tile-based parallelism
// ============================================================================

// Tile size for parallel rendering
constexpr int TILE_SIZE = 16;

// Pre-processed triangle data in screen space
struct ScreenTriangle {
    std::array<HMM_Vec3, 3> screen_verts;  // Screen space positions
    std::array<HMM_Vec4, 3> clip_verts;     // Original clip space (for perspective correction)
    std::array<HMM_Vec2, 3> texcoords;
    std::array<HMM_Vec3, 3> normals;
    float area;
    int min_x, max_x, min_y, max_y;  // Bounding box in pixels
    bool valid;
};

class Rasterizer {
public:
    Framebuffer& fb;
    const Texture* texture = nullptr;
    HMM_Vec3 light_dir;
    
    Rasterizer(Framebuffer& framebuffer) : fb(framebuffer) {
        light_dir = HMM_NormV3(HMM_V3(0.5f, 1.0f, 0.8f));
    }
    
    void set_texture(const Texture* tex) {
        texture = tex;
    }
    
    // Transform triangle to screen space and compute bounding box
    ScreenTriangle prepare_triangle(
        const std::array<HMM_Vec4, 3>& clip_verts,
        const std::array<HMM_Vec2, 3>& texcoords,
        const std::array<HMM_Vec3, 3>& normals
    ) const {
        ScreenTriangle tri;
        tri.clip_verts = clip_verts;
        tri.texcoords = texcoords;
        tri.normals = normals;
        tri.valid = true;
        
        // Convert to screen space
        for (int i = 0; i < 3; i++) {
            float w = clip_verts[i].W;
            if (w <= 0.001f) {
                tri.valid = false;
                return tri;
            }
            
            float x = clip_verts[i].X / w;
            float y = clip_verts[i].Y / w;
            float z = clip_verts[i].Z / w;
            
            tri.screen_verts[i].X = (x + 1.0f) * 0.5f * fb.width;
            tri.screen_verts[i].Y = (1.0f - y) * 0.5f * fb.height;
            tri.screen_verts[i].Z = z;
        }
        
        // Compute bounding box
        float min_x = std::min({tri.screen_verts[0].X, tri.screen_verts[1].X, tri.screen_verts[2].X});
        float max_x = std::max({tri.screen_verts[0].X, tri.screen_verts[1].X, tri.screen_verts[2].X});
        float min_y = std::min({tri.screen_verts[0].Y, tri.screen_verts[1].Y, tri.screen_verts[2].Y});
        float max_y = std::max({tri.screen_verts[0].Y, tri.screen_verts[1].Y, tri.screen_verts[2].Y});
        
        tri.min_x = std::max(0, static_cast<int>(std::floor(min_x)));
        tri.max_x = std::min(fb.width - 1, static_cast<int>(std::ceil(max_x)));
        tri.min_y = std::max(0, static_cast<int>(std::floor(min_y)));
        tri.max_y = std::min(fb.height - 1, static_cast<int>(std::ceil(max_y)));
        
        // Compute area
        auto edge = [](const HMM_Vec3& a, const HMM_Vec3& b, float px, float py) {
            return (px - a.X) * (b.Y - a.Y) - (py - a.Y) * (b.X - a.X);
        };
        tri.area = edge(tri.screen_verts[0], tri.screen_verts[1], 
                        tri.screen_verts[2].X, tri.screen_verts[2].Y);
        
        if (std::abs(tri.area) < 0.001f) {
            tri.valid = false;
        }
        
        return tri;
    }
    
    // Rasterize a triangle within a specific tile region
    void rasterize_triangle_in_tile(
        const ScreenTriangle& tri,
        int tile_x0, int tile_y0, int tile_x1, int tile_y1,
        std::vector<Color>& tile_colors,
        std::vector<float>& tile_depths,
        int tile_width
    ) const {
        if (!tri.valid) return;
        
        // Check if triangle overlaps with tile
        if (tri.max_x < tile_x0 || tri.min_x > tile_x1 ||
            tri.max_y < tile_y0 || tri.min_y > tile_y1) {
            return;
        }
        
        // Clamp to tile bounds
        int x0 = std::max(tri.min_x, tile_x0);
        int x1 = std::min(tri.max_x, tile_x1);
        int y0 = std::max(tri.min_y, tile_y0);
        int y1 = std::min(tri.max_y, tile_y1);
        
        auto edge = [](const HMM_Vec3& a, const HMM_Vec3& b, float px, float py) {
            return (px - a.X) * (b.Y - a.Y) - (py - a.Y) * (b.X - a.X);
        };
        
        float inv_area = 1.0f / tri.area;
        
        for (int y = y0; y <= y1; y++) {
            for (int x = x0; x <= x1; x++) {
                float px = x + 0.5f;
                float py = y + 0.5f;
                
                float w0 = edge(tri.screen_verts[1], tri.screen_verts[2], px, py);
                float w1 = edge(tri.screen_verts[2], tri.screen_verts[0], px, py);
                float w2 = edge(tri.screen_verts[0], tri.screen_verts[1], px, py);
                
                bool inside = (w0 >= 0 && w1 >= 0 && w2 >= 0) || (w0 <= 0 && w1 <= 0 && w2 <= 0);
                if (!inside) continue;
                
                w0 *= inv_area;
                w1 *= inv_area;
                w2 *= inv_area;
                
                float depth = w0 * tri.screen_verts[0].Z + w1 * tri.screen_verts[1].Z + w2 * tri.screen_verts[2].Z;
                if (depth < -1.0f || depth > 1.0f) continue;
                
                // Tile-local index
                int local_x = x - tile_x0;
                int local_y = y - tile_y0;
                int local_idx = local_y * tile_width + local_x;
                
                // Depth test within tile (no atomics needed!)
                if (depth >= tile_depths[local_idx]) continue;
                tile_depths[local_idx] = depth;
                
                // Perspective-correct interpolation
                float inv_w0 = 1.0f / tri.clip_verts[0].W;
                float inv_w1 = 1.0f / tri.clip_verts[1].W;
                float inv_w2 = 1.0f / tri.clip_verts[2].W;
                float inv_w = w0 * inv_w0 + w1 * inv_w1 + w2 * inv_w2;
                float corr = 1.0f / inv_w;
                
                HMM_Vec2 uv;
                uv.X = (w0 * tri.texcoords[0].X * inv_w0 + w1 * tri.texcoords[1].X * inv_w1 + w2 * tri.texcoords[2].X * inv_w2) * corr;
                uv.Y = (w0 * tri.texcoords[0].Y * inv_w0 + w1 * tri.texcoords[1].Y * inv_w1 + w2 * tri.texcoords[2].Y * inv_w2) * corr;
                
                HMM_Vec3 normal;
                normal.X = (w0 * tri.normals[0].X * inv_w0 + w1 * tri.normals[1].X * inv_w1 + w2 * tri.normals[2].X * inv_w2) * corr;
                normal.Y = (w0 * tri.normals[0].Y * inv_w0 + w1 * tri.normals[1].Y * inv_w1 + w2 * tri.normals[2].Y * inv_w2) * corr;
                normal.Z = (w0 * tri.normals[0].Z * inv_w0 + w1 * tri.normals[1].Z * inv_w1 + w2 * tri.normals[2].Z * inv_w2) * corr;
                normal = HMM_NormV3(normal);
                
                Color base_color = texture ? texture->sample(uv.X, uv.Y) : Color(200, 200, 200);
                
                float ndotl = std::max(0.0f, HMM_DotV3(normal, light_dir));
                float lighting = 0.3f + 0.7f * ndotl;
                
                tile_colors[local_idx] = base_color * lighting;
            }
        }
    }
    
    // Render all triangles using tile-based parallelism
    void render_tiled(const std::vector<ScreenTriangle>& triangles) {
        int tiles_x = (fb.width + TILE_SIZE - 1) / TILE_SIZE;
        int tiles_y = (fb.height + TILE_SIZE - 1) / TILE_SIZE;
        int num_tiles = tiles_x * tiles_y;
        
        // Process tiles in parallel
        parallelutil::parallel_for(num_tiles, [&](int tile_idx) {
            int tile_col = tile_idx % tiles_x;
            int tile_row = tile_idx / tiles_x;
            
            int tile_x0 = tile_col * TILE_SIZE;
            int tile_y0 = tile_row * TILE_SIZE;
            int tile_x1 = std::min(tile_x0 + TILE_SIZE - 1, fb.width - 1);
            int tile_y1 = std::min(tile_y0 + TILE_SIZE - 1, fb.height - 1);
            
            int tile_width = tile_x1 - tile_x0 + 1;
            int tile_height = tile_y1 - tile_y0 + 1;
            int tile_size = tile_width * tile_height;
            
            // Local buffers for this tile
            std::vector<Color> tile_colors(tile_size, Color(20, 20, 30));
            std::vector<float> tile_depths(tile_size, std::numeric_limits<float>::max());
            
            // Rasterize all triangles into this tile
            for (const auto& tri : triangles) {
                rasterize_triangle_in_tile(tri, tile_x0, tile_y0, tile_x1, tile_y1,
                                           tile_colors, tile_depths, tile_width);
            }
            
            // Copy tile results to framebuffer (no race condition - tiles don't overlap)
            for (int ly = 0; ly < tile_height; ly++) {
                for (int lx = 0; lx < tile_width; lx++) {
                    int gx = tile_x0 + lx;
                    int gy = tile_y0 + ly;
                    int local_idx = ly * tile_width + lx;
                    int global_idx = gy * fb.width + gx;
                    
                    fb.color_buffer[global_idx] = tile_colors[local_idx];
                    fb.depth_buffer[global_idx] = tile_depths[local_idx];
                }
            }
        });
    }
};

// ============================================================================
// Terminal output - renders framebuffer to terminal using half-block characters
// ============================================================================

class TerminalRenderer {
public:
    // Render framebuffer to terminal using "▀" character
    // Foreground color = top pixel, Background color = bottom pixel
    static void render(const Framebuffer& fb) {
        std::string output;
        output.reserve(fb.width * (fb.height / 2) * 40);  // Pre-allocate
        
        // Move cursor to top-left
        output += "\033[H";
        
        // Process two rows at a time
        for (int y = 0; y < fb.height; y += 2) {
            for (int x = 0; x < fb.width; x++) {
                Color top = fb.get_pixel(x, y);
                Color bottom = (y + 1 < fb.height) ? fb.get_pixel(x, y + 1) : Color(0, 0, 0);
                
                // Set foreground (top pixel) and background (bottom pixel) colors
                // Using 24-bit true color ANSI escape sequences
                char buf[64];
                snprintf(buf, sizeof(buf), "\033[38;2;%d;%d;%dm\033[48;2;%d;%d;%dm",
                         top.r, top.g, top.b, bottom.r, bottom.g, bottom.b);
                output += buf;
                output += "\xE2\x96\x80";  // UTF-8 encoding of "▀" (U+2580)
            }
            output += "\033[0m\n";  // Reset colors and newline
        }
        
        std::cout << output << std::flush;
    }
    
    // Clear screen and hide cursor
    static void init() {
#ifdef _WIN32
        // Set console to UTF-8 code page
        SetConsoleOutputCP(CP_UTF8);
        SetConsoleCP(CP_UTF8);
        
        // Enable virtual terminal processing for ANSI escape sequences
        HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD dwMode = 0;
        GetConsoleMode(hOut, &dwMode);
        dwMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
        SetConsoleMode(hOut, dwMode);
#endif
        std::cout << "\033[2J";     // Clear screen
        std::cout << "\033[?25l";   // Hide cursor
        std::cout << std::flush;
    }
    
    // Show cursor and reset
    static void cleanup() {
        std::cout << "\033[?25h";   // Show cursor
        std::cout << "\033[0m";     // Reset colors
        std::cout << std::flush;
    }
};

// ============================================================================
// Main application
// ============================================================================

int main(int argc, char* argv[]) {
    // Default paths
    const char* obj_path = "assets/vokselia_spawn/vokselia_spawn.obj";
    const char* tex_path = "assets/vokselia_spawn/vokselia_spawn.png";
    
    // Allow custom paths from command line
    if (argc >= 2) obj_path = argv[1];
    if (argc >= 3) tex_path = argv[2];
    
    // Load mesh
    Mesh mesh;
    if (!mesh.load_obj(obj_path)) {
        std::cerr << "Failed to load mesh from: " << obj_path << std::endl;
        return 1;
    }
    
    // Load texture
    Texture texture;
    if (!texture.load(tex_path)) {
        std::cerr << "Warning: Failed to load texture, using default color" << std::endl;
    }
    
    // Get mesh bounds for auto-centering
    HMM_Vec3 mesh_center;
    float mesh_scale;
    mesh.get_bounds(mesh_center, mesh_scale);
    
    // Get initial terminal size
    int term_width, term_height;
    get_terminal_size(term_width, term_height);
    
    // Calculate render dimensions
    // term_height includes status rows, subtract them for actual render area
    // Each character row represents 2 pixel rows (using half-block characters)
    int screen_width = term_width;
    int screen_height = std::max(1, term_height - STATUS_ROWS);  // Character rows for rendering
    int pixel_height = screen_height * 2;  // Actual pixel height
    
    // Create framebuffer
    Framebuffer fb(screen_width, pixel_height);
    Rasterizer rasterizer(fb);
    rasterizer.set_texture(&texture);
    
    // Setup projection matrix (will be updated when terminal resizes)
    auto update_projection = [](int w, int h) {
        float aspect = static_cast<float>(w) / h;
        return HMM_Perspective_RH_NO(HMM_AngleDeg(45.0f), aspect, 0.1f, 100.0f);
    };
    HMM_Mat4 projection = update_projection(screen_width, pixel_height);
    
    // ========================================================================
    // Third Person Camera Setup
    // ========================================================================
    // Free camera with position and orientation
    // - WASD: Move forward/backward/left/right (relative to camera direction)
    // - QE: Move up/down
    // - Arrow keys: Look around (yaw/pitch)
    // - R: Reset camera
    
    // Camera control constants
    constexpr float CAM_MOVE_SPEED = 0.15f;    // Movement speed
    constexpr float CAM_ROTATE_SPEED = 0.06f;  // Rotation speed (radians)
    
    // Camera state
    struct Camera {
        HMM_Vec3 position = HMM_V3(0.0f, 1.0f, 3.0f);  // Camera position
        float yaw = 0.0f;                               // Horizontal angle (radians), 0 = looking at -Z
        float pitch = 0.0f;                             // Vertical angle (radians)
        
        // Get forward direction (where camera is looking, in XZ plane)
        HMM_Vec3 get_forward() const {
            return HMM_V3(
                -std::sin(yaw),
                0.0f,
                -std::cos(yaw)
            );
        }
        
        // Get right direction
        HMM_Vec3 get_right() const {
            return HMM_V3(
                std::cos(yaw),
                0.0f,
                -std::sin(yaw)
            );
        }
        
        // Get look direction (includes pitch)
        HMM_Vec3 get_look_direction() const {
            return HMM_V3(
                -std::sin(yaw) * std::cos(pitch),
                std::sin(pitch),
                -std::cos(yaw) * std::cos(pitch)
            );
        }
        
        // Movement functions
        void move_forward(float amount) {
            position = HMM_AddV3(position, HMM_MulV3F(get_forward(), amount));
        }
        
        void move_right(float amount) {
            position = HMM_AddV3(position, HMM_MulV3F(get_right(), amount));
        }
        
        void move_up(float amount) {
            position.Y += amount;
        }
        
        void rotate_yaw(float amount) {
            yaw += amount;
        }
        
        void rotate_pitch(float amount) {
            // Pitch limits: -1.4f to 1.4f (approx ±80 degrees)
            pitch = std::clamp(pitch + amount, -1.4f, 1.4f);
        }
        
        // Build view matrix
        HMM_Mat4 get_view_matrix() const {
            HMM_Vec3 target = HMM_AddV3(position, get_look_direction());
            return HMM_LookAt_RH(position, target, HMM_V3(0, 1, 0));
        }
        
        void reset() {
            position = HMM_V3(0.0f, 1.0f, 3.0f);
            yaw = 0.0f;
            pitch = 0.0f;
        }
    } camera;
    
    // Initialize terminal
    TerminalRenderer::init();
    
    std::cout << "Press Ctrl+C to exit..." << std::endl;
    
    // Animation loop
    auto start_time = std::chrono::high_resolution_clock::now();
    
    while (true) {
        auto current_time = std::chrono::high_resolution_clock::now();
        float elapsed = std::chrono::duration<float>(current_time - start_time).count();
        (void)elapsed;  // Available for animations if needed
        
        // Check if terminal size changed
        int new_term_width, new_term_height;
        get_terminal_size(new_term_width, new_term_height);
        
        if (new_term_width != term_width || new_term_height != term_height) {
            term_width = new_term_width;
            term_height = new_term_height;
            screen_width = term_width;
            screen_height = std::max(1, term_height - STATUS_ROWS);
            pixel_height = screen_height * 2;
            
            fb.resize(screen_width, pixel_height);
            projection = update_projection(screen_width, pixel_height);
            
            // Clear screen to avoid artifacts
            std::cout << "\033[2J" << std::flush;
        }
        
        // Note: fb.clear() not needed - tile-based rendering initializes each tile
        
        // Build model matrix: center mesh and scale to unit size (no rotation - camera orbits instead)
        HMM_Mat4 model = HMM_M4D(1.0f);
        model = HMM_MulM4(model, HMM_Scale(HMM_V3(2.0f / mesh_scale, 2.0f / mesh_scale, 2.0f / mesh_scale)));
        model = HMM_MulM4(model, HMM_Translate(HMM_V3(-mesh_center.X, -mesh_center.Y, -mesh_center.Z)));
        
        // Get view matrix from third person camera
        HMM_Mat4 view = camera.get_view_matrix();
        
        // Combined MVP matrix
        HMM_Mat4 mvp = HMM_MulM4(projection, HMM_MulM4(view, model));
        
        // Normal matrix (for lighting)
        HMM_Mat4 model_view = HMM_MulM4(view, model);
        
        // ================================================================
        // Tile-based parallel rendering
        // ================================================================
        
        // Step 1: Transform all triangles to screen space (can be parallelized)
        int num_triangles = static_cast<int>(mesh.indices.size() / 3);
        std::vector<ScreenTriangle> screen_triangles(num_triangles);
        
        parallelutil::parallel_for(num_triangles, [&](int tri_idx) {
            size_t i = tri_idx * 3;
            std::array<HMM_Vec4, 3> clip_verts;
            std::array<HMM_Vec2, 3> texcoords;
            std::array<HMM_Vec3, 3> normals;
            
            for (int j = 0; j < 3; j++) {
                const Vertex& v = mesh.vertices[mesh.indices[i + j]];
                
                // Transform vertex to clip space
                HMM_Vec4 pos = HMM_V4(v.position.X, v.position.Y, v.position.Z, 1.0f);
                clip_verts[j] = HMM_MulM4V4(mvp, pos);
                
                // Pass through texcoords
                texcoords[j] = v.texcoord;
                
                // Transform normal to view space
                HMM_Vec4 n = HMM_V4(v.normal.X, v.normal.Y, v.normal.Z, 0.0f);
                HMM_Vec4 transformed_n = HMM_MulM4V4(model_view, n);
                normals[j] = HMM_V3(transformed_n.X, transformed_n.Y, transformed_n.Z);
            }
            
            screen_triangles[tri_idx] = rasterizer.prepare_triangle(clip_verts, texcoords, normals);
        });
        
        // Step 2: Render using tile-based parallelism (no atomics needed!)
        rasterizer.render_tiled(screen_triangles);
        
        // Render to terminal
        TerminalRenderer::render(fb);
        
        // Check for keyboard input
        static int screenshot_count = 0;
        while (keyboard_hit()) {
            int ch = get_char();
            switch (ch) {
                // ============================================================
                // Camera Movement (WASD + QE)
                // ============================================================
                
                // Move forward/backward
                case 'w':
                case 'W':
                    camera.move_forward(CAM_MOVE_SPEED);
                    break;
                case 's':
                case 'S':
                    camera.move_forward(-CAM_MOVE_SPEED);
                    break;
                
                // Move left/right (strafe)
                case 'a':
                case 'A':
                    camera.move_right(-CAM_MOVE_SPEED);
                    break;
                case 'd':
                case 'D':
                    camera.move_right(CAM_MOVE_SPEED);
                    break;
                
                // Move up/down
                case 'q':
                case 'Q':
                    camera.move_up(-CAM_MOVE_SPEED);
                    break;
                case 'e':
                case 'E':
                    camera.move_up(CAM_MOVE_SPEED);
                    break;
                
                // ============================================================
                // Camera Rotation (Arrow keys or IJKL)
                // ============================================================
                
                // Look left/right (yaw)
                case 'j':
                case 'J':
                    camera.rotate_yaw(-CAM_ROTATE_SPEED);
                    break;
                case 'l':
                case 'L':
                    camera.rotate_yaw(CAM_ROTATE_SPEED);
                    break;
                
                // Look up/down (pitch)
                case 'i':
                case 'I':
                    camera.rotate_pitch(CAM_ROTATE_SPEED);
                    break;
                case 'k':
                case 'K':
                    camera.rotate_pitch(-CAM_ROTATE_SPEED);
                    break;
                
                // ============================================================
                // Other Controls
                // ============================================================
                
                // Reset camera to default position
                case 'r':
                case 'R':
                    camera.reset();
                    break;
                
                // Screenshot
                case 'p':
                case 'P': {
                    char filename[64];
                    snprintf(filename, sizeof(filename), "screenshot_%03d.png", screenshot_count++);
                    if (fb.save_to_file(filename)) {
                        std::cout << "\033[" << (screen_height + 4) << ";1H";
                        std::cout << "\033[K";  // Clear line
                        std::cout << "Saved: " << filename << std::flush;
                    }
                    break;
                }
            }
        }
        
        // Display FPS
        static int frame_count = 0;
        static float fps_timer = 0;
        static float fps = 0;
        frame_count++;
        
        float delta = elapsed - fps_timer;
        if (delta >= 1.0f) {
            fps = frame_count / delta;
            frame_count = 0;
            fps_timer = elapsed;
        }
        
        // Print status at bottom
        int status_row = screen_height + 2;
        std::cout << "\033[" << status_row << ";1H\033[K";
        std::cout << "FPS: " << static_cast<int>(fps)
                  << "  Vertices: " << mesh.vertices.size()
                  << "  Res: " << screen_width << "x" << pixel_height
                  << std::fixed << std::setprecision(1)
                  << "  Pos: (" << camera.position.X << ", " << camera.position.Y << ", " << camera.position.Z << ")";
        
        std::cout << "\033[" << (status_row + 1) << ";1H\033[K";
        std::cout << "[WASD] Move  [QE] Up/Down  [IJKL] Look  [R] Reset  [P] Screenshot" << std::flush;
    }
    
    TerminalRenderer::cleanup();
    return 0;
}
