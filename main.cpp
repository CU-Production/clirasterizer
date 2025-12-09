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
#include <conio.h>
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

// ============================================================================
// Platform-specific keyboard input
// ============================================================================

#ifdef _WIN32
// Windows: use _kbhit() and _getch() from conio.h
inline bool keyboard_hit() { return _kbhit() != 0; }
inline int get_char() { return _getch(); }
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
#endif

// ============================================================================
// Configuration
// ============================================================================

constexpr int SCREEN_WIDTH = 120;
constexpr int SCREEN_HEIGHT = 60;  // Actual pixel height = SCREEN_HEIGHT * 2

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
        std::fill(color_buffer.begin(), color_buffer.end(), Color(20, 20, 30));
        std::fill(depth_buffer.begin(), depth_buffer.end(), std::numeric_limits<float>::max());
    }
    
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
// Rasterizer - software triangle rasterization
// ============================================================================

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
    
    // Draw a triangle with interpolated attributes
    void draw_triangle(
        const std::array<HMM_Vec4, 3>& clip_verts,
        const std::array<HMM_Vec2, 3>& texcoords,
        const std::array<HMM_Vec3, 3>& normals
    ) {
        // Convert to screen space
        std::array<HMM_Vec3, 3> screen_verts;
        for (int i = 0; i < 3; i++) {
            // Perspective division
            float w = clip_verts[i].W;
            if (w <= 0.001f) return;  // Behind camera
            
            float x = clip_verts[i].X / w;
            float y = clip_verts[i].Y / w;
            float z = clip_verts[i].Z / w;
            
            // NDC to screen space
            screen_verts[i].X = (x + 1.0f) * 0.5f * fb.width;
            screen_verts[i].Y = (1.0f - y) * 0.5f * fb.height;  // Flip Y
            screen_verts[i].Z = z;
        }
        
        // Compute bounding box
        float min_x = std::min({screen_verts[0].X, screen_verts[1].X, screen_verts[2].X});
        float max_x = std::max({screen_verts[0].X, screen_verts[1].X, screen_verts[2].X});
        float min_y = std::min({screen_verts[0].Y, screen_verts[1].Y, screen_verts[2].Y});
        float max_y = std::max({screen_verts[0].Y, screen_verts[1].Y, screen_verts[2].Y});
        
        int x0 = std::max(0, static_cast<int>(std::floor(min_x)));
        int x1 = std::min(fb.width - 1, static_cast<int>(std::ceil(max_x)));
        int y0 = std::max(0, static_cast<int>(std::floor(min_y)));
        int y1 = std::min(fb.height - 1, static_cast<int>(std::ceil(max_y)));
        
        // Edge function coefficients
        auto edge = [](const HMM_Vec3& a, const HMM_Vec3& b, float px, float py) {
            return (px - a.X) * (b.Y - a.Y) - (py - a.Y) * (b.X - a.X);
        };
        
        float area = edge(screen_verts[0], screen_verts[1], screen_verts[2].X, screen_verts[2].Y);
        if (std::abs(area) < 0.001f) return;  // Degenerate triangle
        
        // Rasterize
        for (int y = y0; y <= y1; y++) {
            for (int x = x0; x <= x1; x++) {
                float px = x + 0.5f;
                float py = y + 0.5f;
                
                float w0 = edge(screen_verts[1], screen_verts[2], px, py);
                float w1 = edge(screen_verts[2], screen_verts[0], px, py);
                float w2 = edge(screen_verts[0], screen_verts[1], px, py);
                
                // Check if inside triangle (allow for both winding orders)
                bool inside = (w0 >= 0 && w1 >= 0 && w2 >= 0) || (w0 <= 0 && w1 <= 0 && w2 <= 0);
                if (!inside) continue;
                
                // Barycentric coordinates
                float inv_area = 1.0f / area;
                w0 *= inv_area;
                w1 *= inv_area;
                w2 *= inv_area;
                
                // Interpolate depth
                float depth = w0 * screen_verts[0].Z + w1 * screen_verts[1].Z + w2 * screen_verts[2].Z;
                
                // Depth test
                if (depth < -1.0f || depth > 1.0f) continue;
                
                // Perspective-correct interpolation
                float inv_w0 = 1.0f / clip_verts[0].W;
                float inv_w1 = 1.0f / clip_verts[1].W;
                float inv_w2 = 1.0f / clip_verts[2].W;
                float inv_w = w0 * inv_w0 + w1 * inv_w1 + w2 * inv_w2;
                float corr = 1.0f / inv_w;
                
                // Interpolate texcoords
                HMM_Vec2 uv;
                uv.X = (w0 * texcoords[0].X * inv_w0 + w1 * texcoords[1].X * inv_w1 + w2 * texcoords[2].X * inv_w2) * corr;
                uv.Y = (w0 * texcoords[0].Y * inv_w0 + w1 * texcoords[1].Y * inv_w1 + w2 * texcoords[2].Y * inv_w2) * corr;
                
                // Interpolate normal
                HMM_Vec3 normal;
                normal.X = (w0 * normals[0].X * inv_w0 + w1 * normals[1].X * inv_w1 + w2 * normals[2].X * inv_w2) * corr;
                normal.Y = (w0 * normals[0].Y * inv_w0 + w1 * normals[1].Y * inv_w1 + w2 * normals[2].Y * inv_w2) * corr;
                normal.Z = (w0 * normals[0].Z * inv_w0 + w1 * normals[1].Z * inv_w1 + w2 * normals[2].Z * inv_w2) * corr;
                normal = HMM_NormV3(normal);
                
                // Sample texture
                Color base_color = texture ? texture->sample(uv.X, uv.Y) : Color(200, 200, 200);
                
                // Simple diffuse lighting
                float ndotl = std::max(0.0f, HMM_DotV3(normal, light_dir));
                float ambient = 0.3f;
                float diffuse = 0.7f * ndotl;
                float lighting = ambient + diffuse;
                
                Color final_color = base_color * lighting;
                fb.set_pixel(x, y, final_color, depth);
            }
        }
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
    
    // Create framebuffer (height * 2 for half-block characters)
    Framebuffer fb(SCREEN_WIDTH, SCREEN_HEIGHT * 2);
    Rasterizer rasterizer(fb);
    rasterizer.set_texture(&texture);
    
    // Setup matrices
    float aspect = static_cast<float>(SCREEN_WIDTH) / (SCREEN_HEIGHT * 2);
    HMM_Mat4 projection = HMM_Perspective_RH_NO(HMM_AngleDeg(45.0f), aspect, 0.1f, 100.0f);
    
    // Camera setup - spherical coordinates (yaw, pitch, distance)
    float camera_yaw = 0.0f;        // Horizontal rotation (radians)
    float camera_pitch = 0.2f;      // Vertical rotation (radians), slightly above
    float camera_distance = 2.5f;   // Distance from origin
    bool auto_rotate = true;        // Auto rotation toggle
    float model_rotation = 0.0f;    // Manual model rotation when auto_rotate is off
    
    // Camera control speed
    constexpr float ROTATE_SPEED = 0.1f;
    constexpr float ZOOM_SPEED = 0.2f;
    constexpr float PITCH_MIN = -1.4f;  // Prevent gimbal lock
    constexpr float PITCH_MAX = 1.4f;
    constexpr float DIST_MIN = 0.5f;
    constexpr float DIST_MAX = 10.0f;
    
    // Initialize terminal
    TerminalRenderer::init();
    
    std::cout << "Press Ctrl+C to exit..." << std::endl;
    
    // Animation loop
    auto start_time = std::chrono::high_resolution_clock::now();
    auto last_time = start_time;
    
    while (true) {
        auto current_time = std::chrono::high_resolution_clock::now();
        float elapsed = std::chrono::duration<float>(current_time - start_time).count();
        (void)last_time;  // Reserved for future delta-time based controls
        last_time = current_time;
        
        // Auto rotation
        if (auto_rotate) {
            model_rotation = elapsed * 0.5f;  // Rotation speed
        }
        
        // Clear framebuffer
        fb.clear();
        
        // Build model matrix: center mesh, scale to unit size, rotate
        HMM_Mat4 model = HMM_M4D(1.0f);
        model = HMM_MulM4(model, HMM_Rotate_RH(model_rotation, HMM_V3(0, 1, 0)));  // Rotate around Y
        model = HMM_MulM4(model, HMM_Scale(HMM_V3(2.0f / mesh_scale, 2.0f / mesh_scale, 2.0f / mesh_scale)));
        model = HMM_MulM4(model, HMM_Translate(HMM_V3(-mesh_center.X, -mesh_center.Y, -mesh_center.Z)));
        
        // Calculate camera position from spherical coordinates
        float cam_x = camera_distance * std::cos(camera_pitch) * std::sin(camera_yaw);
        float cam_y = camera_distance * std::sin(camera_pitch);
        float cam_z = camera_distance * std::cos(camera_pitch) * std::cos(camera_yaw);
        
        // View matrix (camera looking at origin)
        HMM_Mat4 view = HMM_LookAt_RH(
            HMM_V3(cam_x, cam_y, cam_z),  // Camera position (spherical)
            HMM_V3(0, 0, 0),               // Look at origin
            HMM_V3(0, 1, 0)                // Up vector
        );
        
        // Combined MVP matrix
        HMM_Mat4 mvp = HMM_MulM4(projection, HMM_MulM4(view, model));
        
        // Normal matrix (for lighting)
        HMM_Mat4 model_view = HMM_MulM4(view, model);
        
        // Render all triangles
        for (size_t i = 0; i < mesh.indices.size(); i += 3) {
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
            
            rasterizer.draw_triangle(clip_verts, texcoords, normals);
        }
        
        // Render to terminal
        TerminalRenderer::render(fb);
        
        // Check for keyboard input
        static int screenshot_count = 0;
        while (keyboard_hit()) {
            int ch = get_char();
            switch (ch) {
                // Screenshot
                case 'p':
                case 'P': {
                    char filename[64];
                    snprintf(filename, sizeof(filename), "screenshot_%03d.png", screenshot_count++);
                    if (fb.save_to_file(filename)) {
                        std::cout << "\033[" << (SCREEN_HEIGHT + 4) << ";1H";
                        std::cout << "\033[K";  // Clear line
                        std::cout << "Saved: " << filename << std::flush;
                    }
                    break;
                }
                
                // Camera rotation (yaw)
                case 'a':
                case 'A':
                    camera_yaw -= ROTATE_SPEED;
                    break;
                case 'd':
                case 'D':
                    camera_yaw += ROTATE_SPEED;
                    break;
                
                // Camera pitch
                case 'w':
                case 'W':
                    camera_pitch = std::min(camera_pitch + ROTATE_SPEED, PITCH_MAX);
                    break;
                case 's':
                case 'S':
                    camera_pitch = std::max(camera_pitch - ROTATE_SPEED, PITCH_MIN);
                    break;
                
                // Camera zoom
                case 'q':
                case 'Q':
                case '-':
                case '_':
                    camera_distance = std::min(camera_distance + ZOOM_SPEED, DIST_MAX);
                    break;
                case 'e':
                case 'E':
                case '+':
                case '=':
                    camera_distance = std::max(camera_distance - ZOOM_SPEED, DIST_MIN);
                    break;
                
                // Toggle auto rotation
                case ' ':
                    auto_rotate = !auto_rotate;
                    if (!auto_rotate) {
                        model_rotation = elapsed * 0.5f;  // Freeze at current angle
                    }
                    break;
                
                // Manual model rotation (when auto_rotate is off)
                case 'j':
                case 'J':
                    if (!auto_rotate) model_rotation -= ROTATE_SPEED;
                    break;
                case 'l':
                case 'L':
                    if (!auto_rotate) model_rotation += ROTATE_SPEED;
                    break;
                
                // Reset camera
                case 'r':
                case 'R':
                    camera_yaw = 0.0f;
                    camera_pitch = 0.2f;
                    camera_distance = 2.5f;
                    auto_rotate = true;
                    break;
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
        std::cout << "\033[" << (SCREEN_HEIGHT + 2) << ";1H\033[K";
        std::cout << "FPS: " << static_cast<int>(fps) 
                  << "  Verts: " << mesh.vertices.size()
                  << "  Dist: " << std::fixed << std::setprecision(1) << camera_distance
                  << "  Auto: " << (auto_rotate ? "ON" : "OFF");
        
        std::cout << "\033[" << (SCREEN_HEIGHT + 3) << ";1H\033[K";
        std::cout << "[WASD] Camera  [QE] Zoom  [Space] Auto  [JL] Rotate  [R] Reset  [P] Screenshot" << std::flush;
    }
    
    TerminalRenderer::cleanup();
    return 0;
}
