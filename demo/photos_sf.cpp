#include <cassert>
#include <cstdlib>
#include <cstdio>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <fstream>
#include <mutex>
#include <numeric>
#include <queue>
#include <string>
#include <sstream>
#include <thread>
#include <vector>

#ifdef _MSC_VER
#include "win/dirent.h"
#else // _MSC_VER
#include <dirent.h>
#endif

#ifdef _MSC_VER
#pragma warning( push )
#pragma warning( disable : 4312 )
#endif  // _MSC_VER
#include "stb_image.h"
#ifdef _MSC_VER
#pragma warning( pop )
#endif  // _MSC_VER

#include "gpu.h"
#include "decoder.h"

#include "ctpl/ctpl_stl.h"

#ifdef __APPLE__
#  define GLFW_INCLUDE_GLCOREARB 1
#  define GL_GLEXT_PROTOTYPES 1
#  define GLFW_INCLUDE_GLEXT 1
#  include <GLFW/glfw3.h>
#  include <OpenGL/opengl.h>
#elif defined (_WIN32)
#  include <GL/glew.h>
#  include <GLFW/glfw3.h>
#else
#  define GL_GLEXT_PROTOTYPES 1
#  define GLFW_INCLUDE_GLEXT 1
#  include <GLFW/glfw3.h>
#  include <GL/glx.h>
#endif

#include "gl_guards.h"

#define GLIML_NO_PVR
#include "gliml/gliml.h"

#define CRND_HEADER_FILE_ONLY
#include "crn_decomp.h"

static const int kWindowWidth = 512;
static const int kWindowHeight = 512;
static const float kAspect = static_cast<float>(kWindowWidth) / static_cast<float>(kWindowHeight);

static void GetNumSubWindows2D(size_t count, size_t aspect, size_t *nx, size_t *ny) {
  *nx = 1;
  *ny = count;
  while ((*ny % 2) == 0 && 2 * aspect * *nx < *ny) {
    *ny /= 2;
    *nx *= 2;
  }
}

static void GetCropWindow(size_t num, size_t count, float aspect,
                          float *x1, float *x2, float *y1, float *y2) {
  size_t nx = 0;
  size_t ny = 0;

  if (aspect < 1.f) {
    size_t inva = static_cast<size_t>(1.f / aspect);
    GetNumSubWindows2D(count, inva, &nx, &ny);
  } else {
    GetNumSubWindows2D(count, static_cast<size_t>(aspect), &ny, &nx);
  }

  // Compute x and y pixel sample range for sub window
  size_t xo = num % nx;
  size_t yo = num / nx;

  *x1 = static_cast<float>(xo) / static_cast<float>(nx);
  *x2 = static_cast<float>(xo + 1) / static_cast<float>(nx);

  *y1 = static_cast<float>(yo) / static_cast<float>(ny);
  *y2 = static_cast<float>(yo + 1) / static_cast<float>(ny);
}

static void error_callback(int error, const char* description)
{
    fputs(description, stderr);
}

static bool gPaused = false;
static void key_callback(GLFWwindow* window, int key, int scancode, int action, int mods)
{
  if ((key == GLFW_KEY_ESCAPE || key == GLFW_KEY_Q) && action == GLFW_PRESS)
    glfwSetWindowShouldClose(window, GL_TRUE);
  if ((key == GLFW_KEY_P) && action == GLFW_PRESS)
    gPaused = !gPaused;
}

const char *kVertexProg =
  "#version 110\n"
  ""
  "attribute vec3 position;\n"
  "attribute vec2 texCoord;\n"
  ""
  "varying vec2 uv;\n"
  ""
  "void main() {\n"
  "  gl_Position = vec4(position, 1.0);\n"
  "  uv = texCoord;\n"
  "}\n";

const char *kFragProg =
  "#version 110\n"
  ""
  "varying vec2 uv;\n"
  "uniform sampler2D tex;\n"
  ""
  "void main() {\n"
  "  gl_FragColor = vec4(texture2D(tex, uv).rgb, 1);\n"
  "}\n";

GLuint LoadShaders() {
  CHECK_GL_AND_RETURN(GLuint, vertShdrID, glCreateShader, GL_VERTEX_SHADER);
  CHECK_GL_AND_RETURN(GLuint, fragShdrID, glCreateShader, GL_FRAGMENT_SHADER);

  CHECK_GL(glShaderSource, vertShdrID, 1, &kVertexProg , NULL);
  CHECK_GL(glCompileShader, vertShdrID);

  int result, logLength;
  
  CHECK_GL(glGetShaderiv, vertShdrID, GL_COMPILE_STATUS, &result);
  if (result != GL_TRUE) {
    CHECK_GL(glGetShaderiv, vertShdrID, GL_INFO_LOG_LENGTH, &logLength);
    std::vector<char> VertexShaderErrorMessage(logLength);
    CHECK_GL(glGetShaderInfoLog, vertShdrID, logLength, NULL, &VertexShaderErrorMessage[0]);
    fprintf(stdout, "%s\n", &VertexShaderErrorMessage[0]);
    fprintf(stdout, "Vertex shader compilation failed!\n");
    exit(1);
  }

  // Compile Fragment Shader
  CHECK_GL(glShaderSource, fragShdrID, 1, &kFragProg, NULL);
  CHECK_GL(glCompileShader, fragShdrID);

  // Check Fragment Shader
  CHECK_GL(glGetShaderiv, fragShdrID, GL_COMPILE_STATUS, &result);
  if (result != GL_TRUE) {
    CHECK_GL(glGetShaderiv, fragShdrID, GL_INFO_LOG_LENGTH, &logLength);
    std::vector<char> FragmentShaderErrorMessage(logLength);
    CHECK_GL(glGetShaderInfoLog, fragShdrID, logLength, NULL, &FragmentShaderErrorMessage[0]);
    fprintf(stdout, "%s\n", &FragmentShaderErrorMessage[0]);
    fprintf(stdout, "Fragment shader compilation failed!\n");
    exit(1);
  }

  // Link the program
  CHECK_GL_AND_RETURN(GLuint, prog, glCreateProgram);
  CHECK_GL(glAttachShader, prog, vertShdrID);
  CHECK_GL(glAttachShader, prog, fragShdrID);
  CHECK_GL(glLinkProgram, prog);

  // Check the program
  CHECK_GL(glGetProgramiv, prog, GL_LINK_STATUS, &result);
  if (result != GL_TRUE) {
    CHECK_GL(glGetProgramiv, prog, GL_INFO_LOG_LENGTH, &logLength);
    std::vector<char> ProgramErrorMessage( std::max(logLength, int(1)) );
    CHECK_GL(glGetProgramInfoLog, prog, logLength, NULL, &ProgramErrorMessage[0]);
    fprintf(stdout, "%s\n", &ProgramErrorMessage[0]);
  }

  CHECK_GL(glDeleteShader, vertShdrID);
  CHECK_GL(glDeleteShader, fragShdrID);

  return prog;
}

static std::vector<uint8_t> LoadFile(const std::string &filePath) {
  // Load in compressed data.
  std::ifstream is (filePath.c_str(), std::ifstream::binary);
  if (!is) {
    std::cerr << "Error opening GenTC texture: " << filePath << std::endl;
    exit(EXIT_FAILURE);
  }

  is.seekg(0, is.end);
  size_t length = static_cast<size_t>(is.tellg());
  is.seekg(0, is.beg);

  std::vector<uint8_t> cmp_data(length);
  is.read(reinterpret_cast<char *>(cmp_data.data()), length);
  assert(is);
  is.close();
  return std::move(cmp_data);
}

class Texture {
private:
  GLuint _id;
  GLuint _vtx_buffer;
  GLuint _uv_buffer;

  GLint _tex_loc;
  GLint _pos_loc;
  GLint _uv_loc;

public:
  Texture(GLint texLoc, GLint posLoc, GLint uvLoc,
          GLuint texID, size_t num, size_t count)
    : _id(texID), _tex_loc(texLoc), _pos_loc(posLoc), _uv_loc(uvLoc) {
    float x1, x2, y1, y2;
    GetCropWindow(num, count, kAspect, &x1, &x2, &y1, &y2);

    const GLfloat g_FullScreenQuad[] = {
      x1 * 2.0f - 1.0f, y1 * 2.0f - 1.0f, 0.0f,
      x2 * 2.0f - 1.0f, y1 * 2.0f - 1.0f, 0.0f,
      x1 * 2.0f - 1.0f, y2 * 2.0f - 1.0f, 0.0f,
      x2 * 2.0f - 1.0f, y2 * 2.0f - 1.0f, 0.0f
    };

    CHECK_GL(glGenBuffers, 1, &_vtx_buffer);

    CHECK_GL(glBindBuffer, GL_ARRAY_BUFFER, _vtx_buffer);
    CHECK_GL(glBufferData, GL_ARRAY_BUFFER, sizeof(g_FullScreenQuad), g_FullScreenQuad, GL_STATIC_DRAW);
    CHECK_GL(glBindBuffer, GL_ARRAY_BUFFER, 0);

    static const GLfloat g_FullScreenUVs[] = {
      0.0f, 1.0f, 1.0f, 1.0f,
      0.0f, 0.0f, 1.0f, 0.0f
    };

    CHECK_GL(glGenBuffers, 1, &_uv_buffer);

    CHECK_GL(glBindBuffer, GL_ARRAY_BUFFER, _uv_buffer);
    CHECK_GL(glBufferData, GL_ARRAY_BUFFER, sizeof(g_FullScreenUVs), g_FullScreenUVs, GL_STATIC_DRAW);
    CHECK_GL(glBindBuffer, GL_ARRAY_BUFFER, 0);
  }

  ~Texture() {
    CHECK_GL(glDeleteTextures, 1, &_id);
    CHECK_GL(glDeleteBuffers, 1, &_vtx_buffer);
    CHECK_GL(glDeleteBuffers, 1, &_uv_buffer);
  }

  void Draw() const {
    CHECK_GL(glActiveTexture, GL_TEXTURE0);
    CHECK_GL(glBindTexture, GL_TEXTURE_2D, _id);
    CHECK_GL(glUniform1i, _tex_loc, 0);

    CHECK_GL(glEnableVertexAttribArray, _pos_loc);
    CHECK_GL(glBindBuffer, GL_ARRAY_BUFFER, _vtx_buffer);
    CHECK_GL(glVertexAttribPointer, _pos_loc, 3, GL_FLOAT, GL_FALSE, 0, NULL);
    CHECK_GL(glBindBuffer, GL_ARRAY_BUFFER, 0);

    CHECK_GL(glEnableVertexAttribArray, _uv_loc);
    CHECK_GL(glBindBuffer, GL_ARRAY_BUFFER, _uv_buffer);
    CHECK_GL(glVertexAttribPointer, _uv_loc, 2, GL_FLOAT, GL_FALSE, 0, NULL);
    CHECK_GL(glBindBuffer, GL_ARRAY_BUFFER, 0);

    CHECK_GL(glDrawArrays, GL_TRIANGLE_STRIP, 0, 4);

    CHECK_GL(glDisableVertexAttribArray, _pos_loc);
    CHECK_GL(glDisableVertexAttribArray, _uv_loc);
    CHECK_GL(glBindTexture, GL_TEXTURE_2D, 0);
  }
};

// Requesting thread sets sz to a size, dst to a valid pointer, and waits on the cv
// Producing thread sets sz to zero, places a requested pbo in dst, and then notifies the cv.
struct PBORequest {
  size_t sz;
  size_t off;

  size_t in_sz;
  const uint8_t *input;

  GenTC::GenTCHeader *hdr;

  GLuint pbo;
};

class AsyncTexRequest {
 public:
  virtual ~AsyncTexRequest() { }
  bool Run(std::function<void()> *ret) {
    if (_fns.empty()) {
      return false;
    }

    *ret = _fns.front();
    _fns.pop();
    return true;
  }

  void QueueWork(std::function<void()> fn) {
    _fns.push(fn);
  }

  virtual GLuint TextureHandle() const = 0;
  virtual bool NeedsPBO(PBORequest **ret) = 0;
  virtual void LoadTexture() const = 0;
 private:
  std::queue<std::function<void()> > _fns;
};

class AsyncGenTCReq : public AsyncTexRequest {
 public:
  AsyncGenTCReq(const std::unique_ptr<gpu::GPUContext> &ctx, GLuint id)
    : AsyncTexRequest()
    , _ctx(ctx)
    , _texID(id)
    , _queue(ctx->GetNextQueue())
  { }
  virtual ~AsyncGenTCReq() { }

  virtual GLuint TextureHandle() const override { return _texID; }

  virtual bool NeedsPBO(PBORequest **ret) override {
    *ret = &_pbo;
    return true;
  }

  virtual void LoadTexture() const override {
    // Initialize the texture...
    CHECK_GL(glBindBuffer, GL_PIXEL_UNPACK_BUFFER, _pbo.pbo);
    CHECK_GL(glBindTexture, GL_TEXTURE_2D, _texID);
    CHECK_GL(glCompressedTexImage2D, GL_TEXTURE_2D, 0, GL_COMPRESSED_RGB_S3TC_DXT1_EXT,
             static_cast<GLsizei>(_hdr.width), static_cast<GLsizei>(_hdr.height), 0,
             static_cast<GLsizei>(_pbo.sz), reinterpret_cast<const void *>(_pbo.off));
    CHECK_GL(glTexParameteri, GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    CHECK_GL(glTexParameteri, GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    CHECK_GL(glTexParameteri, GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    CHECK_GL(glTexParameteri, GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    CHECK_GL(glTexParameteri, GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    CHECK_GL(glTexParameteri, GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);

    CHECK_GL(glBindBuffer, GL_PIXEL_UNPACK_BUFFER, 0);
    CHECK_GL(glBindTexture, GL_TEXTURE_2D, 0);
  }

  virtual void Preload(const std::string &fname) {
    // Load in compressed data.
    std::ifstream is(fname.c_str(), std::ifstream::binary);
    if (!is) {
      std::cerr << "Error opening GenTC texture: " << fname << std::endl;
      exit(EXIT_FAILURE);
    }

    is.seekg(0, is.end);
    size_t length = static_cast<size_t>(is.tellg());
    is.seekg(0, is.beg);

    static const size_t kHeaderSz = sizeof(_hdr);
    const size_t mem_sz = length - kHeaderSz;

    is.read(reinterpret_cast<char *>(&_hdr), kHeaderSz);

    _cmp_data.resize(mem_sz);
    is.read(reinterpret_cast<char *>(_cmp_data.data()), _cmp_data.size());

    assert(is);
    assert(is.tellg() == static_cast<std::streamoff>(length));
    is.close();

    _pbo.sz = (_hdr.width * _hdr.height) / 2;
    _pbo.in_sz = _cmp_data.size();
    _pbo.input = _cmp_data.data();
    _pbo.hdr = &_hdr;
  }

 private:
  const std::unique_ptr<gpu::GPUContext> &_ctx;
  GLuint _texID;
  cl_command_queue _queue;

  std::vector<uint8_t> _cmp_data;
  GenTC::GenTCHeader _hdr;
  cl_mem _cmp_buf_host;
  cl_mem _cmp_buf;
  cl_event _write_event;
  PBORequest _pbo;
};

class AsyncGenericReq : public AsyncTexRequest {
 public:
  AsyncGenericReq(GLuint id)
    : AsyncTexRequest()
    , _texID(id)
  { }

  virtual GLuint TextureHandle() const override { return _texID; }
  virtual void LoadTexture() const override {
    assert(this->_n == 3);

    // Initialize the texture...
    CHECK_GL(glBindTexture, GL_TEXTURE_2D, _texID);
    CHECK_GL(glTexImage2D, GL_TEXTURE_2D, 0, GL_RGB8, _x, _y, 0, GL_RGB, GL_UNSIGNED_BYTE, this->_data);
    CHECK_GL(glTexParameteri, GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    CHECK_GL(glTexParameteri, GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    CHECK_GL(glTexParameteri, GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    CHECK_GL(glTexParameteri, GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    CHECK_GL(glTexParameteri, GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    CHECK_GL(glTexParameteri, GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    CHECK_GL(glBindTexture, GL_TEXTURE_2D, 0);
    stbi_image_free(this->_data);
  }

  virtual bool NeedsPBO(PBORequest **ret) override { return false; }
  virtual void LoadFile(const std::string &filename) {
    this->_data = stbi_load(filename.c_str(), &_x, &_y, &_n, 3);
    assert(_n == 3);  // Only load RGB textures...
    _n = 3;
  }

 protected:
  const GLuint _texID;
 private:
  int _x, _y, _n;
  unsigned char *_data;
};

class AsyncGLIMLReq : public AsyncGenericReq {
public:
  AsyncGLIMLReq(GLuint id) : AsyncGenericReq(id) { }

  virtual void LoadTexture() const override {
    gliml::context gliml_ctx;
    gliml_ctx.enable_dxt(true);

    if (!gliml_ctx.load(_ktx_data.data(), static_cast<int>(_ktx_data.size()))) {
      std::cerr << "Error reading GLIML file!" << std::endl;
      exit(EXIT_FAILURE);
    }

    assert(gliml_ctx.num_faces() == 1);
    assert(gliml_ctx.num_mipmaps(0) == 1);
    assert(gliml_ctx.is_2d());

    // Initialize the texture...
    CHECK_GL(glBindTexture, gliml_ctx.texture_target(), _texID);
    if (gliml_ctx.is_compressed()) {
      CHECK_GL(glCompressedTexImage2D, gliml_ctx.texture_target(), 0,
                                       gliml_ctx.image_internal_format(),
                                       gliml_ctx.image_width(0, 0),
                                       gliml_ctx.image_height(0, 0), 0,
                                       gliml_ctx.image_size(0, 0),
                                       gliml_ctx.image_data(0, 0));
    } else {
      CHECK_GL(glTexImage2D, gliml_ctx.texture_target(), 0,
                             gliml_ctx.image_internal_format(),
                             gliml_ctx.image_width(0, 0),
                             gliml_ctx.image_height(0, 0), 0,
                             gliml_ctx.image_format(), gliml_ctx.image_type(),
                             gliml_ctx.image_data(0, 0));
    }
    CHECK_GL(glTexParameteri, gliml_ctx.texture_target(), GL_TEXTURE_BASE_LEVEL, 0);
    CHECK_GL(glTexParameteri, gliml_ctx.texture_target(), GL_TEXTURE_MAX_LEVEL, 0);
    CHECK_GL(glTexParameteri, gliml_ctx.texture_target(), GL_TEXTURE_WRAP_S, GL_REPEAT);
    CHECK_GL(glTexParameteri, gliml_ctx.texture_target(), GL_TEXTURE_WRAP_T, GL_REPEAT);
    CHECK_GL(glTexParameteri, gliml_ctx.texture_target(), GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    CHECK_GL(glTexParameteri, gliml_ctx.texture_target(), GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  }

  virtual void LoadFile(const std::string &filename) override {
    _ktx_data = std::move(::LoadFile(filename));
  }

private:
  std::vector<uint8_t> _ktx_data;
};

class AsyncCrunchReq : public AsyncGenericReq {
public:
  AsyncCrunchReq(GLuint id): AsyncGenericReq(id) { }

  virtual void LoadTexture() const override {
    // Initialize the texture...
    GLsizei dxt_sz = static_cast<GLsizei>(_dxt_data.size());
    CHECK_GL(glBindTexture, GL_TEXTURE_2D, _texID);
    CHECK_GL(glCompressedTexImage2D, GL_TEXTURE_2D, 0, GL_COMPRESSED_RGB_S3TC_DXT1_EXT,
                                     _width, _height, 0, dxt_sz, _dxt_data.data());
    CHECK_GL(glTexParameteri, GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    CHECK_GL(glTexParameteri, GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    CHECK_GL(glTexParameteri, GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    CHECK_GL(glTexParameteri, GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    CHECK_GL(glTexParameteri, GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    CHECK_GL(glTexParameteri, GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  }

  virtual void LoadFile(const std::string &filename) override {
    std::vector<uint8_t> crn_data = std::move(::LoadFile(filename));
    crnd::uint32 crn_data_sz = static_cast<crnd::uint32>(crn_data.size());

    crnd::crn_texture_info tinfo;
    if (!crnd::crnd_get_texture_info(crn_data.data(), crn_data_sz, &tinfo)) {
      assert(!"Invalid texture?");
      return;
    }

    crnd::crnd_unpack_context ctx = crnd::crnd_unpack_begin(crn_data.data(), crn_data_sz);
    if (!ctx) {
      assert(!"Error beginning crn decoding!");
      return;
    }

    _width = tinfo.m_width;
    _height = tinfo.m_height;

    const int num_blocks_x = (tinfo.m_width + 3) / 4;
    const int num_blocks_y = (tinfo.m_height + 3) / 4;
    const int num_blocks = num_blocks_x * num_blocks_y;

    _dxt_data.resize(num_blocks * 8);
    void *dxt_data = reinterpret_cast<void *>(_dxt_data.data());
    if (!crnd::crnd_unpack_level(ctx, &dxt_data, num_blocks * 8, num_blocks_x * 8, 0)) {
      assert(!"Error decoding crunch texture!");
      return;
    }

    crnd::crnd_unpack_end(ctx);
  }

private:
  GLsizei _width, _height;
  std::vector<uint8_t> _dxt_data;
};

std::vector<std::unique_ptr<Texture> > LoadTextures(const std::unique_ptr<gpu::GPUContext> &ctx,
                                                    GLint texLoc, GLint posLoc, GLint uvLoc,
                                                    bool async, const char *dirname) {
  // Load textures!
  DIR *dir = opendir(dirname);
  if (!dir) {
    std::cerr << "Error opening directory " << dirname << std::endl;
    exit(EXIT_FAILURE);
  }

  // Collect the actual filenames
  std::vector<std::string> filenames;
  struct dirent *entry = NULL;
  while ((entry = readdir(dir)) != NULL) {
    // A few exceptions...
    if (strlen(entry->d_name) == 1 && strncmp(entry->d_name, ".", 1) == 0) continue;
    if (strlen(entry->d_name) == 2 && strncmp(entry->d_name, "..", 2) == 0) continue;
    filenames.push_back(std::string(dirname) + std::string("/") + std::string(entry->d_name));
  }
  closedir(dir);

  // We'll have as many textures as we have filenames
  std::vector<std::unique_ptr<Texture> > textures;
  textures.reserve(filenames.size());

  // Load up a bunch of requests
  std::vector<std::unique_ptr<AsyncTexRequest> > reqs;
  reqs.reserve(textures.size());

  // Events that we need to wait on before we release GL objects...
  std::mutex dxt_events_mutex;
  std::vector<cl_event> dxt_events;

  for (size_t i = 0; i < filenames.size(); ++i) {

#ifndef NDEBUG
    std::cout << "Loading texture: " << filenames[i] << std::endl;
#endif

    GLuint texID;
    CHECK_GL(glGenTextures, 1, &texID);

    size_t len = filenames[i].length();
    assert(len >= 4);
    if (strncmp(filenames[i].c_str() + len - 4, ".gtc", 4) == 0) {
      reqs.push_back(std::unique_ptr<AsyncTexRequest>(new AsyncGenTCReq(ctx, texID)));
      AsyncTexRequest *req = reqs.back().get();
      const std::string &fname = filenames[i];

      // Pre-pbo
      req->QueueWork([&fname, req]() {
        reinterpret_cast<AsyncGenTCReq *>(req)->Preload(fname);
      });
    } else {
      if (strncmp(filenames[i].c_str() + len - 4, ".ktx", 4) == 0 ||
          strncmp(filenames[i].c_str() + len - 4, ".dds", 4) == 0) {
        reqs.push_back(std::unique_ptr<AsyncTexRequest>(new AsyncGLIMLReq(texID)));
      } else if (strncmp(filenames[i].c_str() + len - 4, ".crn", 4) == 0) {
        reqs.push_back(std::unique_ptr<AsyncTexRequest>(new AsyncCrunchReq(texID)));
      } else {
        reqs.push_back(std::unique_ptr<AsyncTexRequest>(new AsyncGenericReq(texID)));
      }

      AsyncTexRequest *req = reqs.back().get();
      const std::string &fname = filenames[i];
      req->QueueWork([req, &fname]() {
        reinterpret_cast<AsyncGenericReq *>(req)->LoadFile(fname);
      });
    }

    textures.push_back(std::unique_ptr<Texture>(
                       new Texture(texLoc, posLoc, uvLoc, texID, i, filenames.size())));
  }

  // Loop until all requests are dun:
  //   - Run all of the requests that need it.
  //   - Collect GL/CL interop resources for each request

  std::atomic_int num_loaded; num_loaded = 0;
  std::condition_variable loading_cv;

  const unsigned kTotalNumThreads = async ? std::thread::hardware_concurrency() : 1;
  ctpl::thread_pool pool(kTotalNumThreads);

  std::chrono::time_point<std::chrono::high_resolution_clock> start, end;
  double idle_time = 0.0;
  double interop_time = 0.0;

  // Queue up work
  std::mutex m;
  std::condition_variable done;
  std::atomic_int num_finished(0);
  int num_running = 0;
  for (auto &req : reqs) {
    std::function<void()> fn;
    if (req->Run(&fn)) {
      pool.push([fn, &num_finished, &done, &m](int) {
        fn();
        std::unique_lock<std::mutex> lock(m);
        num_finished++;
        done.notify_one();
      });
      num_running++;
    }
  }

  // Wait for the work to finish
  start = std::chrono::high_resolution_clock::now(); {
    std::unique_lock<std::mutex> lock(m);
    done.wait(lock, [&]() { return num_running == num_finished; });
  } end = std::chrono::high_resolution_clock::now();
  idle_time += std::chrono::duration<double>(end-start).count();

  // Collect all of our pbo requests if we need to acquire them
  std::vector<PBORequest *> pbo_reqs;
  pbo_reqs.reserve(textures.size());
  GLsizeiptr total_pbo_size = 0;
  for (auto &req : reqs) {
    PBORequest *pbo_req;
    if (req->NeedsPBO(&pbo_req)) {
      pbo_reqs.push_back(pbo_req);
      assert((pbo_req->sz % 512) == 0);
      total_pbo_size += pbo_req->sz;
    }
  }

  GLuint pbo;
  if (total_pbo_size > 0) {
    cl_int errCreateBuffer;
    cl_context cl_ctx = ctx->GetOpenCLContext();
    cl_command_queue d_queue = ctx->GetDefaultCommandQueue();

    start = std::chrono::high_resolution_clock::now();
    CHECK_GL(glGenBuffers, 1, &pbo);
    CHECK_GL(glBindBuffer, GL_PIXEL_UNPACK_BUFFER, pbo);
    CHECK_GL(glBufferData, GL_PIXEL_UNPACK_BUFFER, total_pbo_size, NULL, GL_STREAM_COPY);

    // Wait for the GPU to finish
    CHECK_GL(glFlush);
    CHECK_GL(glFinish);

    // Get the PBO
    cl_mem pbo_cl = clCreateFromGLBuffer(cl_ctx, CL_MEM_WRITE_ONLY, pbo, &errCreateBuffer);
    CHECK_CL((cl_int), errCreateBuffer);
    end = std::chrono::high_resolution_clock::now();
    interop_time += std::chrono::duration<double>(end - start).count();

    static const size_t kPageSize = 16;
    const size_t kNumPages = pbo_reqs.size() / kPageSize;
    std::vector<size_t> input_sizes;
    input_sizes.reserve(pbo_reqs.size() / kPageSize);

    size_t out_mem_sz = 0;
    size_t input_mem_sz = 0;
    for (size_t i = 0; i < pbo_reqs.size(); ++i) {
      if (i % kPageSize == 0) {
        input_sizes.push_back(input_mem_sz);
        input_mem_sz += ((kPageSize * 4 * 4 * 2 + 511) / 512) * 512;
      }
      input_mem_sz += pbo_reqs[i]->in_sz;

      out_mem_sz += GenTC::RequiredScratchMem(*(pbo_reqs[i]->hdr));
    }

    GenTC::PreallocateDecompressor(ctx, out_mem_sz);

    // Create pinned host memory and device memory
    cl_mem_flags pinned_flags = CL_MEM_READ_WRITE | CL_MEM_ALLOC_HOST_PTR;
    cl_mem cmp_buf_host = clCreateBuffer(cl_ctx, pinned_flags, input_mem_sz, NULL, &errCreateBuffer);
    CHECK_CL((cl_int), errCreateBuffer);

    // Map the host memory to the application's address space...
    void *pinned_mem = clEnqueueMapBuffer(d_queue, cmp_buf_host, CL_TRUE, CL_MAP_WRITE | CL_MAP_READ,
                                          0, input_mem_sz, 0, NULL, NULL, &errCreateBuffer);
    CHECK_CL((cl_int), errCreateBuffer);

    cl_event user_event = clCreateUserEvent(cl_ctx, &errCreateBuffer);
    CHECK_CL((cl_int), errCreateBuffer);

    // Unmap and enqueue copy
    cl_event unmap_event;
    CHECK_CL(clEnqueueUnmapMemObject, d_queue, cmp_buf_host, pinned_mem, 1, &user_event, &unmap_event);

    start = std::chrono::high_resolution_clock::now();
    cl_event acquire_event;
    CHECK_CL(clEnqueueAcquireGLObjects, d_queue, 1, &pbo_cl, 1, &user_event, &acquire_event);
    end = std::chrono::high_resolution_clock::now();
    interop_time += std::chrono::duration<double>(end - start).count();

    // Queue up a few pages of work
    std::vector<cl_event> dxt_events;
    dxt_events.reserve(2 * pbo_reqs.size());

    num_finished.store(0);
    num_running = 0;

    size_t page_id = 0;
    auto page_start = pbo_reqs.begin();
    auto page_end = pbo_reqs.begin() + kPageSize;
    for (auto input_sz : input_sizes) {

      if (page_id > 0) {
        page_start = page_end;
        page_end = std::min(page_end + kPageSize, pbo_reqs.end());
      }

      pool.push([page_start, page_end, pinned_mem, input_sz, page_id, unmap_event, user_event, kNumPages,
                 acquire_event, cmp_buf_host, pbo_cl, &dxt_events, &m, &done, &num_finished, &ctx](int) {
        size_t num_hdrs = static_cast<size_t>(page_end - page_start);
        cl_uint num_blocks = static_cast<cl_uint>((*page_start)->hdr->width * (*page_start)->hdr->height / 16);
        uint8_t *page_buf = reinterpret_cast<uint8_t *>(pinned_mem) + input_sz;

        cl_uint *offsets_buf = reinterpret_cast<cl_uint *>(page_buf);
        cl_uint *input_offsets = offsets_buf + 4 * num_hdrs;
        cl_uint *output_offsets = offsets_buf;
        size_t offsets_sz = (((num_hdrs * 4 * 2) + 127) / 128) * 128;

        uint8_t *freqs_buf = reinterpret_cast<uint8_t *>(offsets_buf + offsets_sz);
        uint8_t *data_buf = freqs_buf + 4 * 512 * num_hdrs;

        size_t output_offset_idx = 0;
        size_t input_offset_idx = 0;

        cl_uint output_offset = 0;
        cl_uint input_offset = 0;

        std::vector<GenTC::GenTCHeader> hdrs(num_hdrs);
        auto next_hdr = hdrs.begin();
        for (auto it = page_start; it != page_end; it++) {
          auto req = *it;

          const uint8_t *in_mem = reinterpret_cast<const uint8_t *>(req->input);
          memcpy(freqs_buf, in_mem, 4 * 512);
          in_mem += 4 * 512;
          freqs_buf += 4 * 512;

          size_t req_sz = req->in_sz - 4 * 512;
          memcpy(data_buf, in_mem, req_sz);
          data_buf += req_sz;

          // Setup ANS input offsets
          input_offsets[input_offset_idx++] = input_offset; input_offset += req->hdr->y_cmp_sz;
          input_offsets[input_offset_idx++] = input_offset; input_offset += req->hdr->chroma_cmp_sz;
          input_offsets[input_offset_idx++] = input_offset; input_offset += req->hdr->palette_sz;
          input_offsets[input_offset_idx++] = input_offset; input_offset += req->hdr->indices_sz;

          // Setup ANS output offsets
          output_offsets[output_offset_idx++] = output_offset; output_offset += 2 * num_blocks; // Y planes
          output_offsets[output_offset_idx++] = output_offset; output_offset += 4 * num_blocks; // Chroma planes
          output_offsets[output_offset_idx++] = output_offset; output_offset += static_cast<cl_uint>(req->hdr->palette_bytes); // Palette
          output_offsets[output_offset_idx++] = output_offset; output_offset += num_blocks; // Indices

          memcpy(&(*next_hdr), req->hdr, sizeof(GenTC::GenTCHeader));
          next_hdr++;
        }

        cl_int errCreateBuffer;
        cl_context cl_ctx = ctx->GetOpenCLContext();
        cl_command_queue queue = ctx->GetNextQueue();

        size_t cmp_buf_sz = (offsets_sz * 4) + input_offset + num_hdrs * 4 * 512;
        cl_mem cmp_buf = clCreateBuffer(cl_ctx, CL_MEM_READ_ONLY, cmp_buf_sz, NULL, &errCreateBuffer);
        CHECK_CL((cl_int), errCreateBuffer);

        cl_event copy_event;
        CHECK_CL(clEnqueueCopyBuffer, queue, cmp_buf_host, cmp_buf, input_sz, 0, cmp_buf_sz,
                                      1, &unmap_event, &copy_event);


        size_t kPageSizeBytes = kPageSize * num_blocks * 8;
        cl_buffer_region dst_region;
        dst_region.origin = page_id * kPageSizeBytes;
        dst_region.size = kPageSizeBytes;
        assert((dst_region.origin % (ctx->GetDeviceInfo<cl_uint>(CL_DEVICE_MEM_BASE_ADDR_ALIGN) / 8)) == 0);

        cl_mem dst = clCreateSubBuffer(pbo_cl, CL_MEM_WRITE_ONLY, CL_BUFFER_CREATE_TYPE_REGION,
                                       &dst_region, &errCreateBuffer);
        CHECK_CL((cl_int), errCreateBuffer);

        cl_event init_events[2] = { acquire_event, copy_event };
        cl_event ret_event = GenTC::LoadCompressedDXTs(ctx, hdrs, queue, cmp_buf, dst, 2, init_events);
        CHECK_CL(clReleaseEvent, copy_event);
        CHECK_CL(clReleaseMemObject, cmp_buf);
        CHECK_CL(clReleaseMemObject, dst);

        std::unique_lock<std::mutex> lock(m);
        dxt_events.push_back(ret_event);
        num_finished++;
        done.notify_one();
      });

      num_running++;
      page_id++;
    }

    // Wait for work to finish
    {
      std::unique_lock<std::mutex> lock(m);
      done.wait(lock, [&]() { return num_running == num_finished; });
    }

    // Go go go!
    CHECK_CL(clSetUserEventStatus, user_event, CL_COMPLETE);

    CHECK_CL(clReleaseEvent, user_event);
    CHECK_CL(clReleaseEvent, unmap_event);
    CHECK_CL(clReleaseEvent, acquire_event);
    CHECK_CL(clReleaseMemObject, cmp_buf_host);

    // Acquire all of the CL buffers at once...
    std::cout << "Loading textures acquire GL time: " << interop_time << "s" << std::endl;

    // Set the event for all the requests so that they know
    // that it's ok to use it, and create the subbuffer to write into
    size_t output_offset = 0;
    for (auto req : pbo_reqs) {
      // Create
      req->pbo = pbo;
      req->off = output_offset;
      output_offset += req->sz;
    }

    // We're done, let's do the rest of the work...
    start = std::chrono::high_resolution_clock::now();
    cl_event release_event;
    CHECK_CL(clEnqueueReleaseGLObjects, ctx->GetDefaultCommandQueue(), 1, &pbo_cl,
                                        static_cast<cl_uint>(dxt_events.size()), dxt_events.data(),
                                        &release_event);
    end = std::chrono::high_resolution_clock::now();
    interop_time += std::chrono::duration<double>(end - start).count();

    for (cl_event e : dxt_events) {
      CHECK_CL(clReleaseEvent, e);
    }
    CHECK_CL(clReleaseMemObject, pbo_cl);

    // Wait for the OpenCL event to finish...
    start = std::chrono::high_resolution_clock::now();
    CHECK_CL(clWaitForEvents, 1, &release_event);
    end = std::chrono::high_resolution_clock::now();
    idle_time += std::chrono::duration<double>(end-start).count();
    CHECK_CL(clReleaseEvent, release_event);

    GenTC::FreeDecompressor();
  }

  // I think we're done now...
  for (auto &req : reqs) {
    req->LoadTexture();
  }

  std::cout << "Loading textures idle time: " << idle_time << "s" << std::endl;
  std::cout << "Loading textures interop time: " << interop_time << "s" << std::endl;
  CHECK_GL(glDeleteBuffers, 1, &pbo);
  return std::move(textures);
}

int main(int argc, char* argv[] ) {
    if (argc <= 1) {
      std::cerr << "Usage: " << argv[0] << " [-p] <directory>" << std::endl;
      exit(EXIT_FAILURE);
    }

    bool profiling = false;
    bool async = true;

    uint32_t next_arg = 1;
    for (;;) {
      if (strncmp(argv[next_arg], "-p", 3) == 0) {
        profiling = true;
      }
      else if (strncmp(argv[next_arg], "-s", 3) == 0) {
        async = false;
      }
      else {
        break;
      }
      next_arg++;
    }

    const char *dirname = argv[next_arg];

    glfwSetErrorCallback(error_callback);
    if (!glfwInit())
      exit(EXIT_FAILURE);

    GLFWwindow* window = glfwCreateWindow(512, 512, "Photos", NULL, NULL);
    if (!window) {
      glfwTerminate();
      exit(EXIT_FAILURE);
    }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    fprintf(stdout, "GL Vendor: %s\n", glGetString(GL_VENDOR));
    fprintf(stdout, "GL Renderer: %s\n", glGetString(GL_RENDERER));
    fprintf(stdout, "GL Version: %s\n", glGetString(GL_VERSION));
    fprintf(stdout, "GL Shading Language Version: %s\n",
            glGetString(GL_SHADING_LANGUAGE_VERSION));

#ifndef NDEBUG
    std::string extensionsString(reinterpret_cast<const char *>(glGetString(GL_EXTENSIONS)));
    std::vector<char> extensionVector(extensionsString.begin(), extensionsString.end());
    for (size_t i = 0; i < extensionVector.size(); ++i) {
      if (extensionVector[i] == ',' || extensionVector[i] == ' ') {
        extensionVector[i] = '\0';
      }
    }

    fprintf(stdout, "GL extensions:\n");
    fprintf(stdout, "  %s\n", &(extensionVector[0]));
    for (size_t i = 1; i < extensionVector.size(); i++) {
      if (extensionVector[i] == '\0' && i < extensionVector.size() - 1) {
        fprintf(stdout, "  %s\n", &(extensionVector[i + 1]));
      }
    }
#endif

#ifdef _WIN32
    if (GLEW_OK != glewInit()) {
      std::cerr << "Failed to initialize glew!" << std::endl;
      exit(1);
    }
#endif

    std::unique_ptr<gpu::GPUContext> ctx = gpu::GPUContext::InitializeOpenCL(true);
    if (!GenTC::InitializeDecoder(ctx)) {
      std::cerr << "ERROR: OpenCL device does not support features needed for decoder." << std::endl;
      exit(EXIT_FAILURE);
    }

    glfwSetKeyCallback(window, key_callback);

    GLuint prog = LoadShaders();
    CHECK_GL_AND_RETURN(GLint, posLoc, glGetAttribLocation, prog, "position");
    CHECK_GL_AND_RETURN(GLint, uvLoc, glGetAttribLocation, prog, "texCoord");
    CHECK_GL_AND_RETURN(GLint, texLoc, glGetUniformLocation, prog, "tex");

    assert ( posLoc >= 0 );
    assert ( uvLoc >= 0 );
    assert ( texLoc >= 0 );

    std::chrono::time_point<std::chrono::high_resolution_clock> start, end;
    start = std::chrono::high_resolution_clock::now();
    std::vector<std::unique_ptr<Texture> > texs =
      LoadTextures(ctx, texLoc, posLoc, uvLoc, async, dirname);
    end = std::chrono::high_resolution_clock::now();
    std::cout << "Loaded " << texs.size() << " texture"
              << ((texs.size() == 1) ? "" : "s") << " in "
              << std::chrono::duration<double>(end-start).count() << "s"
              << std::endl;
    
    CHECK_GL(glPixelStorei, GL_UNPACK_ALIGNMENT, 1);

    static const int kFrameTimeHistorySz = 8;
    double frame_times[kFrameTimeHistorySz] = { 0 };
    int frame_time_idx = 0;
    double elapsed_since_refresh = 0.0;

    while (!glfwWindowShouldClose(window)) {
      double start_time = glfwGetTime();
      glfwPollEvents();

      if (gPaused) {
        continue;
      }

      int width, height;
      glfwGetFramebufferSize(window, &width, &height);
      CHECK_GL(glViewport, 0, 0, width, height);

      CHECK_GL(glClear, GL_COLOR_BUFFER_BIT);
      CHECK_GL(glUseProgram, prog);
      for (const auto &tex : texs) {
        tex->Draw();
      }

      glfwSwapBuffers(window);
      if (profiling) {
        glfwSetWindowShouldClose(window, GL_TRUE);
      }

      double end_time = glfwGetTime();
      frame_times[frame_time_idx] = (end_time - start_time) * 1000.0;
      frame_time_idx = (frame_time_idx + 1) % kFrameTimeHistorySz;
      elapsed_since_refresh += (end_time - start_time);

      if (elapsed_since_refresh > 1.0) {
        double time = std::accumulate(frame_times, frame_times + kFrameTimeHistorySz, 0.0);
        double frame_time = time / static_cast<double>(kFrameTimeHistorySz);
        double fps = 1000.0 / frame_time;

        std::cout << '\r';
        std::cout << "FPS: " << fps;
        std::cout.flush();
        elapsed_since_refresh = 0.0;
      }
    }
    std::cout << std::endl;

    // Finish GPU things
    clFlush(ctx->GetDefaultCommandQueue());
    clFinish(ctx->GetDefaultCommandQueue());

    // Delete OpenCL crap before we destroy everything else...
    ctx = nullptr;

    CHECK_GL(glFlush);
    CHECK_GL(glFinish);

    CHECK_GL(glDeleteProgram, prog);

    glfwDestroyWindow(window);

    glfwTerminate();
    exit(EXIT_SUCCESS);
}

//! [code]
