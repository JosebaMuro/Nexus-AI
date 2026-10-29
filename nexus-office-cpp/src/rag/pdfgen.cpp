// Maquetacion de documentos extraidos en un PDF estructurado (rag/pdfgen.py, alli con fpdf2).
//
// Portada, resumen y puntos clave, indice con paginas (y marcadores PDF), contenido por secciones (parrafos, listas,
// tablas, codigo, citas), galeria de imagenes, enlaces y metadatos. Escritor PDF propio y sin dependencias: fuentes
// base (Helvetica y Courier) en WinAnsiEncoding, que cubren el espanol y el resto de idiomas de Europa occidental;
// lo que no cabe en esa codificacion sale como «?». Imagenes en JPEG (stb), contenido comprimido (miniz).
#include <cmath>
#include <cstdint>
#include <map>

#include "miniz.h"
#include "rag/web.h"
#include "stb_image.h"
#include "stb_image_resize.h"
#include "stb_image_write.h"
#include "util/crypto.h"
#include "util/http.h"
#include "util/rx.h"
#include "util/text.h"

namespace nx::rag {

namespace {

#include "pdf_widths.inc"

struct RGB {
  int r, g, b;
};
const RGB ACCENT{0, 168, 196}, DARK{4, 6, 12}, INK{24, 32, 44}, MUTED{110, 128, 148};

// UTF-8 -> WinAnsi (cp1252); lo que no existe en ella pasa a '?'
std::string winansi(const std::string& s) {
  static const std::map<char32_t, unsigned char> HIGH = {
      {0x20AC, 0x80}, {0x201A, 0x82}, {0x0192, 0x83}, {0x201E, 0x84}, {0x2026, 0x85}, {0x2020, 0x86}, {0x2021, 0x87},
      {0x02C6, 0x88}, {0x2030, 0x89}, {0x0160, 0x8A}, {0x2039, 0x8B}, {0x0152, 0x8C}, {0x017D, 0x8E}, {0x2018, 0x91},
      {0x2019, 0x92}, {0x201C, 0x93}, {0x201D, 0x94}, {0x2022, 0x95}, {0x2013, 0x96}, {0x2014, 0x97}, {0x02DC, 0x98},
      {0x2122, 0x99}, {0x0161, 0x9A}, {0x203A, 0x9B}, {0x0153, 0x9C}, {0x017E, 0x9E}, {0x0178, 0x9F}};
  std::string out;
  for (char32_t c : u32(s)) {
    if (c == '\t') c = ' ';
    if (c < 0x20 && c != '\n') continue;
    if ((c >= 0x200B && c <= 0x200F) || c == 0xFEFF) continue;
    if (c < 0x80 || (c >= 0xA0 && c <= 0xFF)) out += (char)c;
    else if (auto it = HIGH.find(c); it != HIGH.end()) out += (char)it->second;
    else if (c == 0x2212) out += '-';
    else if (c == 0x00A0 || c == 0x2009 || c == 0x202F) out += ' ';
    else out += '?';
  }
  return out;
}

std::string pdf_str(const std::string& bytes) {   // literal (...) con escapes
  std::string o = "(";
  for (unsigned char c : bytes) {
    if (c == '(' || c == ')' || c == '\\') o += '\\', o += (char)c;
    else if (c == '\r') o += "\\r";
    else if (c == '\n') o += "\\n";
    else o += (char)c;
  }
  return o + ")";
}

std::string utf16_hex(const std::string& s) {   // cadenas de texto del documento (titulo, marcadores)
  std::string o = "<FEFF";
  char buf[8];
  for (char32_t c : u32(s)) {
    auto put = [&](unsigned v) {
      std::snprintf(buf, sizeof buf, "%04X", v);
      o += buf;
    };
    if (c >= 0x10000) {
      c -= 0x10000;
      put(0xD800 + (unsigned)(c >> 10));
      put(0xDC00 + (unsigned)(c & 0x3FF));
    } else {
      put((unsigned)c);
    }
  }
  return o + ">";
}

std::string f2(double v) {
  char b[32];
  std::snprintf(b, sizeof b, "%.2f", v);
  return b;
}

std::string deflate(const std::string& data) {
  mz_ulong n = mz_compressBound((mz_ulong)data.size());
  std::string out(n, '\0');
  if (mz_compress2((unsigned char*)out.data(), &n, (const unsigned char*)data.data(), (mz_ulong)data.size(), 6) != MZ_OK)
    return "";
  out.resize(n);
  return out;
}

struct Image {
  std::string jpeg;
  int w, h;
};

// Escritor PDF minimo al estilo de FPDF: milimetros, origen arriba a la izquierda
class Pdf {
 public:
  const double k = 72.0 / 25.4, W = 210, H = 297;
  double l = 18, t = 18, r = 18, b = 18, x = 18, y = 18, c_margin = 1, lasth = 0;
  bool auto_break = true;
  int style = 0;   // 0 normal, 1 negrita, 2 cursiva, 3 mono
  double size = 12;
  std::string title, source;

  struct Link {
    double x, y, w, h;
    std::string uri;
    int page = -1;
    double dest_y = 0;
  };
  struct Page {
    std::string content;
    std::vector<Link> links;
    double after_header = 18;
  };
  struct Section {
    std::string name;
    int level, page;
    double y;
  };
  std::vector<Page> pages;
  std::vector<Section> sections;
  std::vector<Image> images;
  int cur = -1;
  std::map<std::string, std::string> info;
  bool in_toc = false;
  int toc_first = -1, toc_count = 0;

  double fs() const { return size / k; }   // tamano de la fuente en mm
  std::string& out() { return pages[cur].content; }
  double width(const std::string& s) const {   // s ya en WinAnsi
    double w = 0;
    int f = style == 3 ? 3 : style;
    for (unsigned char c : s) w += WIDTHS[f][c];
    return w * size / 1000.0 / k;
  }
  int page_no() const { return cur + 1; }

  void set_font(int st, double sz) {
    style = st;
    size = sz;
  }
  RGB text_rgb{0, 0, 0};
  void text_color(RGB c) { text_rgb = c; }
  void fill_color(RGB c) { out() += f2(c.r / 255.0) + " " + f2(c.g / 255.0) + " " + f2(c.b / 255.0) + " rg\n"; }
  void draw_color(RGB c) { out() += f2(c.r / 255.0) + " " + f2(c.g / 255.0) + " " + f2(c.b / 255.0) + " RG\n"; }
  void line_width(double w) { out() += f2(w * k) + " w\n"; }
  void rect(double x0, double y0, double w, double h, bool fill) {
    out() += f2(x0 * k) + " " + f2((H - y0) * k) + " " + f2(w * k) + " " + f2(-h * k) + (fill ? " re f\n" : " re S\n");
  }
  void line(double x1, double y1, double x2, double y2) {
    out() += f2(x1 * k) + " " + f2((H - y1) * k) + " m " + f2(x2 * k) + " " + f2((H - y2) * k) + " l S\n";
  }

  void add_page() {
    if (cur >= 0 && !in_toc) footer();
    pages.push_back({});
    cur = (int)pages.size() - 1;
    x = l;
    y = t;
    int st = style;
    double sz = size;
    header();
    set_font(st, sz);
    pages[cur].after_header = y;
  }

  void text_at(double tx, double ty, const std::string& s) {
    out() += "BT /F" + std::to_string(style + 1) + " " + f2(size) + " Tf " + f2(text_rgb.r / 255.0) + " " +
             f2(text_rgb.g / 255.0) + " " + f2(text_rgb.b / 255.0) + " rg " + f2(tx * k) + " " + f2((H - ty) * k) +
             " Td " + pdf_str(s) + " Tj ET\n";
  }

  bool need_break(double h) const { return auto_break && y + h > H - b && !in_toc; }

  // una linea: w=0 hasta el margen derecho; align L, C o R; next: false = sigue a la derecha
  void cell(double w, double h, const std::string& txt_utf8, char align = 'L', bool next = false,
            const std::string& uri = "", bool fill = false, int link_page = -1, bool raw = false) {
    std::string txt = raw ? txt_utf8 : winansi(txt_utf8);
    if (need_break(h)) {
      double keep = x;
      add_page();
      x = keep;
    }
    if (w == 0) w = W - r - x;
    if (fill) rect(x, y, w, h, true);
    if (!txt.empty()) {
      double tw = width(txt);
      double dx = align == 'R' ? w - c_margin - tw : align == 'C' ? (w - tw) / 2 : c_margin;
      text_at(x + dx, y + 0.5 * h + 0.3 * fs(), txt);
      if (!uri.empty() || link_page >= 0) pages[cur].links.push_back({x + dx, y + 0.5 * h - 0.5 * fs(), tw, fs(), uri, link_page, 0});
    }
    lasth = h;
    if (next) {
      x = l;
      y += h;
    } else {
      x += w;
    }
  }

  std::vector<std::string> wrap(const std::string& txt, double w) const {   // txt en WinAnsi
    std::vector<std::string> lines;
    double maxw = w - 2 * c_margin;
    for (auto& para : split(txt, "\n")) {
      std::string line;
      for (auto& word : split(para, " ")) {
        std::string cand = line.empty() ? word : line + " " + word;
        if (width(cand) <= maxw || line.empty()) {
          if (width(cand) > maxw) {   // palabra mas larga que la linea: se corta por caracteres
            std::string piece;
            for (char c : cand) {
              if (width(piece + c) > maxw && !piece.empty()) {
                lines.push_back(piece);
                piece.clear();
              }
              piece += c;
            }
            line = piece;
          } else {
            line = cand;
          }
        } else {
          lines.push_back(line);
          line = word;
          if (width(line) > maxw) {
            std::string piece;
            for (char c : line) {
              if (width(piece + c) > maxw && !piece.empty()) {
                lines.push_back(piece);
                piece.clear();
              }
              piece += c;
            }
            line = piece;
          }
        }
      }
      lines.push_back(line);
    }
    return lines;
  }

  // parrafo con saltos de linea automaticos; termina en el margen izquierdo, debajo
  void multi_cell(double w, double h, const std::string& txt_utf8, char align = 'L', bool fill = false,
                  const std::string& uri = "") {
    if (w == 0) w = W - r - x;
    double x0 = x;
    for (auto& ln_ : wrap(winansi(txt_utf8), w)) {
      x = x0;
      if (need_break(h)) {
        add_page();
        x0 = x = std::max(x0, l);
      }
      cell(w, h, ln_, align, false, uri, fill, -1, true);
      x = x0;
      y += h;
    }
    x = l;
  }

  void ln(double h) {
    x = l;
    y += h;
  }

  void start_section(const std::string& name, int level) { sections.push_back({name, level, cur, y}); }

  void image(const Image& im, double ix, double iy, double w, double h) {
    images.push_back(im);
    out() += "q " + f2(w * k) + " 0 0 " + f2(h * k) + " " + f2(ix * k) + " " + f2((H - (iy + h)) * k) + " cm /I" +
             std::to_string(images.size()) + " Do Q\n";
  }

  void header() {
    if (page_no() == 1) return;
    set_font(0, 8);
    text_color(MUTED);
    cell(0, 6, "Nexus-Office · " + uleft(title, 90), 'L', true);
    draw_color(ACCENT);
    line_width(0.4);
    line(l, y, W - r, y);
    ln(4);
  }

  void footer() {
    if (page_no() == 1) return;
    int st = style;
    double sz = size;
    double keep_y = y;
    bool ab = auto_break;
    auto_break = false;
    y = H - 12;
    set_font(0, 8);
    text_color(MUTED);
    cell(0, 6, uleft(source, 100), 'L');
    x = l;
    cell(0, 6, std::to_string(page_no()) + " / {nb}", 'R', false, "", false, -1, true);
    auto_break = ab;
    y = keep_y;
    style = st;
    size = sz;
  }

  std::string output() {
    footer();
    std::string nb = std::to_string(pages.size());
    std::vector<std::string> objs;   // objs[i] es el objeto i+1
    auto reserve = [&] {
      objs.emplace_back();
      return (int)objs.size();
    };
    int catalog = reserve(), pages_obj = reserve();
    static const char* FONTS[4] = {"Helvetica", "Helvetica-Bold", "Helvetica-Oblique", "Courier"};
    std::string fonts;
    for (int i = 0; i < 4; ++i) {
      int f = reserve();
      objs[f - 1] = std::string("<< /Type /Font /Subtype /Type1 /BaseFont /") + FONTS[i] + " /Encoding /WinAnsiEncoding >>";
      fonts += "/F" + std::to_string(i + 1) + " " + std::to_string(f) + " 0 R ";
    }
    std::string xobjs;
    for (size_t i = 0; i < images.size(); ++i) {
      int o = reserve();
      auto& im = images[i];
      objs[o - 1] = "<< /Type /XObject /Subtype /Image /Width " + std::to_string(im.w) + " /Height " + std::to_string(im.h) +
                    " /ColorSpace /DeviceRGB /BitsPerComponent 8 /Filter /DCTDecode /Length " +
                    std::to_string(im.jpeg.size()) + " >>\nstream\n" + im.jpeg + "\nendstream";
      xobjs += "/I" + std::to_string(i + 1) + " " + std::to_string(o) + " 0 R ";
    }
    int resources = reserve();
    objs[resources - 1] = "<< /ProcSet [/PDF /Text /ImageC] /Font << " + fonts + ">> /XObject << " + xobjs + ">> >>";
    std::vector<int> page_ids;
    for (size_t i = 0; i < pages.size(); ++i) page_ids.push_back(reserve());
    std::string kids;
    for (size_t i = 0; i < pages.size(); ++i) {
      std::string content = replace_all(pages[i].content, "{nb}", nb);
      std::string z = deflate(content);
      int c = reserve();
      objs[c - 1] = "<< /Filter /FlateDecode /Length " + std::to_string(z.size()) + " >>\nstream\n" + z + "\nendstream";
      std::string annots;
      for (auto& lk : pages[i].links) {
        int a = reserve();
        std::string rect = f2(lk.x * k) + " " + f2((H - lk.y - lk.h) * k) + " " + f2((lk.x + lk.w) * k) + " " + f2((H - lk.y) * k);
        std::string action = lk.page >= 0 && lk.page < (int)page_ids.size()
                                 ? "/Dest [" + std::to_string(page_ids[lk.page]) + " 0 R /XYZ 0 " + f2(H * k) + " null]"
                                 : "/A << /S /URI /URI " + pdf_str(lk.uri) + " >>";
        objs[a - 1] = "<< /Type /Annot /Subtype /Link /Rect [" + rect + "] /Border [0 0 0] " + action + " >>";
        annots += std::to_string(a) + " 0 R ";
      }
      objs[page_ids[i] - 1] = "<< /Type /Page /Parent " + std::to_string(pages_obj) + " 0 R /MediaBox [0 0 " + f2(W * k) +
                              " " + f2(H * k) + "] /Resources " + std::to_string(resources) + " 0 R /Contents " +
                              std::to_string(c) + " 0 R" + (annots.empty() ? "" : " /Annots [" + annots + "]") + " >>";
      kids += std::to_string(page_ids[i]) + " 0 R ";
    }
    objs[pages_obj - 1] = "<< /Type /Pages /Kids [" + kids + "] /Count " + nb + " >>";
    // marcadores (arbol por niveles)
    std::string outlines_ref;
    if (!sections.empty()) {
      int root = reserve();
      std::vector<int> ids;
      for (size_t i = 0; i < sections.size(); ++i) ids.push_back(reserve());
      std::vector<int> parent(sections.size(), -1);
      std::vector<int> stack;   // indices de secciones abiertas
      for (size_t i = 0; i < sections.size(); ++i) {
        while (!stack.empty() && sections[stack.back()].level >= sections[i].level) stack.pop_back();
        parent[i] = stack.empty() ? -1 : stack.back();
        stack.push_back((int)i);
      }
      auto children = [&](int p) {
        std::vector<int> c;
        for (size_t i = 0; i < sections.size(); ++i)
          if (parent[i] == p) c.push_back((int)i);
        return c;
      };
      std::function<int(int)> count = [&](int p) {
        int n = 0;
        for (int c : children(p)) n += 1 + count(c);
        return n;
      };
      for (size_t i = 0; i < sections.size(); ++i) {
        auto sib = children(parent[i]);
        auto pos = std::find(sib.begin(), sib.end(), (int)i) - sib.begin();
        auto kidsv = children((int)i);
        auto& s = sections[i];
        std::string o = "<< /Title " + utf16_hex(s.name) + " /Parent " +
                        std::to_string(parent[i] < 0 ? root : ids[parent[i]]) + " 0 R";
        if (pos > 0) o += " /Prev " + std::to_string(ids[sib[pos - 1]]) + " 0 R";
        if (pos + 1 < (long)sib.size()) o += " /Next " + std::to_string(ids[sib[pos + 1]]) + " 0 R";
        if (!kidsv.empty())
          o += " /First " + std::to_string(ids[kidsv.front()]) + " 0 R /Last " + std::to_string(ids[kidsv.back()]) +
               " 0 R /Count -" + std::to_string(count((int)i));
        int pg = std::clamp(s.page, 0, (int)page_ids.size() - 1);
        o += " /Dest [" + std::to_string(page_ids[pg]) + " 0 R /XYZ 0 " + f2((H - s.y) * k) + " null] >>";
        objs[ids[i] - 1] = o;
      }
      auto top = children(-1);
      objs[root - 1] = "<< /Type /Outlines /First " + std::to_string(ids[top.front()]) + " 0 R /Last " +
                       std::to_string(ids[top.back()]) + " 0 R /Count " + std::to_string(count(-1)) + " >>";
      outlines_ref = " /Outlines " + std::to_string(root) + " 0 R /PageMode /UseOutlines";
    }
    objs[catalog - 1] = "<< /Type /Catalog /Pages " + std::to_string(pages_obj) + " 0 R" + outlines_ref + " >>";
    int info_id = reserve();
    std::string inf = "<< /Producer " + pdf_str("Nexus-Office") + " /CreationDate " +
                      pdf_str(strftime_local("D:%Y%m%d%H%M%S")) + " ";
    for (auto& [kk, v] : info) inf += "/" + kk + " " + utf16_hex(v) + " ";
    objs[info_id - 1] = inf + ">>";

    std::string pdf = "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n";
    std::vector<size_t> offs;
    for (size_t i = 0; i < objs.size(); ++i) {
      offs.push_back(pdf.size());
      pdf += std::to_string(i + 1) + " 0 obj\n" + objs[i] + "\nendobj\n";
    }
    size_t xref = pdf.size();
    pdf += "xref\n0 " + std::to_string(objs.size() + 1) + "\n0000000000 65535 f \n";
    char buf[32];
    for (size_t o : offs) {
      std::snprintf(buf, sizeof buf, "%010zu 00000 n \n", o);
      pdf += buf;
    }
    pdf += "trailer\n<< /Size " + std::to_string(objs.size() + 1) + " /Root " + std::to_string(catalog) + " 0 R /Info " +
           std::to_string(info_id) + " 0 R /ID [<" + token_hex(16) + "> <" + token_hex(16) + ">] >>\nstartxref\n" +
           std::to_string(xref) + "\n%%EOF\n";
    return pdf;
  }
};

void jpeg_sink(void* ctx, void* data, int size) { static_cast<std::string*>(ctx)->append((const char*)data, size); }

std::optional<Image> fetch_image(const std::string& url) {
  try {
    check_public_url(url);
  } catch (...) {
    return std::nullopt;
  }
  auto r = http::get(url, {{"User-Agent", "Mozilla/5.0 (Nexus-Office)"}}, 10);
  if (r.status != 200 || r.body.size() > 4000000 || !starts_with(r.header("content-type"), "image")) return std::nullopt;
  int w = 0, h = 0, n = 0;
  unsigned char* px = stbi_load_from_memory((const unsigned char*)r.body.data(), (int)r.body.size(), &w, &h, &n, 3);
  if (!px) return std::nullopt;
  if (w < 80 || h < 80) {   // iconos y pixeles de seguimiento
    stbi_image_free(px);
    return std::nullopt;
  }
  std::vector<unsigned char> rgb(px, px + (size_t)w * h * 3);
  stbi_image_free(px);
  if (w > 1400 || h > 1400) {
    double sc = 1400.0 / std::max(w, h);
    int nw = std::max(1, (int)(w * sc)), nh = std::max(1, (int)(h * sc));
    std::vector<unsigned char> small((size_t)nw * nh * 3);
    stbir_resize_uint8(rgb.data(), w, h, 0, small.data(), nw, nh, 0, 3);
    rgb.swap(small);
    w = nw;
    h = nh;
  }
  Image im{"", w, h};
  stbi_write_jpg_to_func(jpeg_sink, &im.jpeg, w, h, 3, rgb.data(), 85);
  if (im.jpeg.empty()) return std::nullopt;
  return im;
}

}  // namespace

json build_pdf(const json& doc, const std::string& summary, const std::vector<std::string>& key_points, const fs::path& out,
               int max_images) {
  std::string title = jstr(doc, "title"), url = jstr(doc, "url");
  Pdf pdf;
  pdf.title = title;
  pdf.source = url;
  pdf.info["Title"] = title;
  pdf.info["Author"] = !jstr(doc, "author").empty() ? jstr(doc, "author") : !jstr(doc, "site").empty() ? jstr(doc, "site") : "Nexus-Office";
  pdf.info["Subject"] = uleft(jstr(doc, "description"), 200);
  std::vector<std::string> heads;
  for (auto& hd : doc.value("headings", json::array())) {
    if (heads.size() >= 12) break;
    heads.push_back(hd.get<std::string>());
  }
  pdf.info["Keywords"] = join(heads, ", ");
  pdf.info["Creator"] = "Nexus-Office";
  auto W = [&] { return pdf.W - pdf.l - pdf.r; };
  const json& sections = doc.value("sections", json::array());
  const json& images = doc.value("images", json::array());
  const json& links = doc.value("links", json::array());

  // ---------------- portada (sin salto automatico: el pie se dibuja dentro del margen inferior)
  pdf.add_page();
  pdf.auto_break = false;
  pdf.fill_color(DARK);
  pdf.rect(0, 0, pdf.W, pdf.H, true);
  pdf.fill_color(ACCENT);
  pdf.rect(0, 0, 6, pdf.H, true);
  pdf.x = 22;
  pdf.y = 40;
  pdf.set_font(1, 11);
  pdf.text_color({0, 240, 255});
  pdf.cell(0, 8, "N E X U S   ·   D O C U M E N T O   E X T R A Í D O", 'L', true);
  pdf.ln(8);
  pdf.x = 22;
  pdf.set_font(1, 26);
  pdf.text_color({235, 250, 255});
  pdf.multi_cell(pdf.W - 44, 12, title);
  if (!jstr(doc, "description").empty()) {
    pdf.ln(4);
    pdf.x = 22;
    pdf.set_font(2, 12);
    pdf.text_color({170, 200, 215});
    pdf.multi_cell(pdf.W - 44, 6.5, uleft(jstr(doc, "description"), 500));
  }
  pdf.y = pdf.H - 95;
  std::vector<std::pair<std::string, std::string>> meta = {
      {"Fuente", url},
      {"Sitio", jstr(doc, "site")},
      {"Autor", jstr(doc, "author")},
      {"Publicado", jstr(doc, "published")},
      {"Idioma", jstr(doc, "lang")},
      {"Extraído", strftime_local("%d/%m/%Y %H:%M")},
      {"Contenido", std::to_string(doc.value("words", 0)) + " palabras · " + std::to_string(sections.size()) + " secciones · " +
                        std::to_string(images.size()) + " imágenes · " + std::to_string(links.size()) + " enlaces"}};
  for (auto& [kk, v] : meta) {
    if (v.empty()) continue;
    pdf.x = 22;
    pdf.set_font(1, 9);
    pdf.text_color({0, 240, 255});
    pdf.cell(28, 6, upper(kk));
    pdf.set_font(0, 9.5);
    pdf.text_color({215, 235, 245});
    pdf.multi_cell(pdf.W - 72, 6, uleft(v, 300), 'L', false, kk == "Fuente" ? url : "");
  }
  pdf.x = 22;
  pdf.y = pdf.H - 22;
  pdf.set_font(0, 8);
  pdf.text_color(MUTED);
  pdf.cell(0, 6, "Generado automáticamente por Nexus-Office · Copia archivada en su biblioteca RAG");
  pdf.auto_break = true;

  // ---------------- indice (se rellena al final, cuando se conocen las paginas)
  pdf.add_page();
  int n_toc = 1;
  for (auto& s : sections)
    if (s.value("level", 1) <= 2) ++n_toc;
  n_toc = 1 + (n_toc - 1) / 34;
  pdf.toc_first = pdf.cur;
  pdf.toc_count = n_toc;
  for (int i = 1; i < n_toc; ++i) pdf.add_page();
  pdf.add_page();

  auto heading = [&](const std::string& text, int level) {
    if (pdf.y > pdf.H - 45) pdf.add_page();
    pdf.start_section(uleft(text, 120), std::max(0, level - 1));
    double sz = level == 1 ? 17 : level == 2 ? 14 : 12;
    pdf.ln(level > 1 ? 3 : 5);
    pdf.set_font(1, sz);
    pdf.text_color(level == 1 ? ACCENT : INK);
    pdf.multi_cell(W(), sz * 0.5, text);
    if (level == 1) {
      pdf.draw_color(ACCENT);
      pdf.line_width(0.6);
      pdf.line(pdf.l, pdf.y + 1, pdf.l + 30, pdf.y + 1);
      pdf.ln(3);
    }
    pdf.ln(1);
  };
  auto para = [&](const std::string& text, bool italic = false) {
    pdf.set_font(italic ? 2 : 0, 10.5);
    pdf.text_color(INK);
    pdf.multi_cell(W(), 5.6, text);
    pdf.ln(2);
  };

  // ---------------- resumen
  heading("Resumen", 1);
  para(summary);
  if (!key_points.empty()) {
    heading("Puntos clave", 2);
    for (auto& kp : key_points) {
      pdf.set_font(0, 10.5);
      pdf.text_color(ACCENT);
      pdf.cell(6, 5.6, "•");
      pdf.text_color(INK);
      pdf.multi_cell(W() - 6, 5.6, kp);
    }
    pdf.ln(2);
  }

  // ---------------- contenido
  heading("Contenido", 1);
  for (auto& sec : sections) {
    const json& blocks = sec.value("blocks", json::array());
    int level = sec.value("level", 1);
    if (blocks.empty() && level >= 3) continue;
    heading(jstr(sec, "heading"), std::min(3, level + 1));
    for (auto& bl : blocks) {
      std::string type = jstr(bl, "type");
      try {
        if (type == "p") {
          para(jstr(bl, "text"));
        } else if (type == "quote") {
          double x0 = pdf.l, y0 = pdf.y;
          pdf.l += 6;
          pdf.x = pdf.l;
          para(jstr(bl, "text"), true);
          pdf.l = x0;
          pdf.x = x0;
          pdf.draw_color(ACCENT);
          if (pdf.y - 2 > y0) pdf.line(x0 + 2, y0, x0 + 2, pdf.y - 2);
        } else if (type == "list") {
          pdf.set_font(0, 10.5);
          int i = 0;
          for (auto& it : bl.value("items", json::array())) {
            if (i >= 200) break;
            std::string item = it.get<std::string>();
            double indent = starts_with(item, "  ") ? 4 : 0;
            pdf.x = pdf.l + indent;
            pdf.text_color(ACCENT);
            pdf.cell(7, 5.6, bl.value("ordered", false) && !indent ? std::to_string(i + 1) + "." : "•");
            pdf.text_color(INK);
            pdf.multi_cell(W() - 7 - indent, 5.6, trim(item));
            ++i;
          }
          pdf.ln(2);
        } else if (type == "code") {
          pdf.set_font(3, 8.5);
          pdf.fill_color({240, 245, 248});
          pdf.text_color({30, 40, 60});
          pdf.multi_cell(W(), 4.4, uleft(jstr(bl, "text"), 6000), 'L', true);
          pdf.ln(2);
        } else if (type == "table") {
          std::vector<std::vector<std::string>> rows;
          size_t ncol = 0;
          for (auto& rw : bl.value("rows", json::array())) {
            if (rows.size() >= 80) break;
            std::vector<std::string> cells;
            for (auto& c : rw) {
              if (cells.size() >= 8) break;
              cells.push_back(winansi(uleft(c.is_string() ? c.get<std::string>() : c.dump(), 300)));
            }
            ncol = std::max(ncol, cells.size());
            rows.push_back(cells);
          }
          if (!ncol) continue;
          pdf.set_font(0, 8.5);
          pdf.text_color(INK);
          pdf.draw_color(INK);
          pdf.line_width(0.2);
          double cw = W() / ncol, lh = 4.6;
          pdf.line(pdf.l, pdf.y, pdf.l + W(), pdf.y);
          for (size_t ri = 0; ri < rows.size(); ++ri) {
            rows[ri].resize(ncol);
            std::vector<std::vector<std::string>> wrapped;
            size_t nl = 1;
            for (auto& c : rows[ri]) {
              wrapped.push_back(pdf.wrap(c, cw));
              nl = std::max(nl, wrapped.back().size());
            }
            double rh = nl * lh + 1;
            if (pdf.need_break(rh)) pdf.add_page();
            double y0 = pdf.y;
            for (size_t ci = 0; ci < ncol; ++ci) {
              pdf.y = y0 + 0.5;
              for (auto& ln_ : wrapped[ci]) {
                pdf.x = pdf.l + ci * cw;
                pdf.cell(cw, lh, ln_, 'L', false, "", false, -1, true);
                pdf.y += lh;
              }
            }
            pdf.y = y0 + rh;
            pdf.x = pdf.l;
            if (ri == 0 && rows.size() > 1) pdf.line(pdf.l, pdf.y, pdf.l + W(), pdf.y);
          }
          pdf.ln(3);
        }
      } catch (const std::exception&) {   // un bloque raro no debe romper el documento
        std::vector<std::string> items;
        for (auto& it : bl.value("items", json::array())) items.push_back(it.get<std::string>());
        para(!items.empty() ? join(items, " ") : jstr(bl, "text"));
      }
    }
  }

  // ---------------- imagenes
  int embedded = 0;
  if (!images.empty() && max_images > 0) {
    heading("Imágenes", 1);
    size_t lim = std::min(images.size(), (size_t)max_images * 2);
    for (size_t i = 0; i < lim && embedded < max_images; ++i) {
      auto im = fetch_image(jstr(images[i], "src"));
      if (!im) continue;
      double w = std::min(W(), 150.0), h = w * im->h / im->w;
      if (h > 150) {
        h = 150;
        w = 150.0 * im->w / im->h;
      }
      if (pdf.y + h + 14 > pdf.H - 18) pdf.add_page();
      pdf.image(*im, pdf.l + (W() - w) / 2, pdf.y, w, h);
      pdf.y += h;
      pdf.set_font(2, 8.5);
      pdf.text_color(MUTED);
      std::string alt = jstr(images[i], "alt");
      pdf.multi_cell(W(), 4.5, uleft(alt.empty() ? jstr(images[i], "src") : alt, 200), 'C');
      pdf.ln(4);
      ++embedded;
    }
    if (!embedded)
      for (size_t i = 0; i < images.size() && i < 20; ++i) {
        std::string alt = jstr(images[i], "alt");
        para((alt.empty() ? std::string("Imagen") : alt) + ": " + jstr(images[i], "src"));
      }
  }

  // ---------------- enlaces y metadatos
  if (!links.empty()) {
    heading("Enlaces", 1);
    pdf.set_font(0, 8.5);
    for (size_t i = 0; i < links.size() && i < 120; ++i) {
      pdf.text_color(INK);
      pdf.multi_cell(W(), 4.6, jstr(links[i], "text"));
      pdf.text_color(ACCENT);
      pdf.multi_cell(W(), 4.2, uleft(jstr(links[i], "href"), 250), 'L', false, jstr(links[i], "href"));
      pdf.ln(1);
    }
  }
  heading("Metadatos", 1);
  meta.push_back({"Palabras clave", jstr(doc, "keywords")});
  meta.push_back({"Generador", "Nexus-Office " NEXUS_VERSION " (C++)"});
  for (auto& [kk, v] : meta)
    if (!v.empty()) para(kk + ": " + v);

  // ---------------- el indice, ya con las paginas de cada seccion
  int last = pdf.cur;
  double keep_y = pdf.y;
  pdf.in_toc = true;
  pdf.cur = pdf.toc_first;
  pdf.y = pdf.pages[pdf.cur].after_header;
  pdf.x = pdf.l;
  pdf.set_font(1, 16);
  pdf.text_color(ACCENT);
  pdf.cell(0, 10, "Índice", 'L', true);
  pdf.ln(2);
  for (auto& s : pdf.sections) {
    if (s.level > 2) continue;
    if (pdf.y + 6.5 > pdf.H - pdf.b) {
      if (pdf.cur + 1 >= pdf.toc_first + pdf.toc_count) break;
      ++pdf.cur;
      pdf.y = pdf.pages[pdf.cur].after_header;
    }
    pdf.set_font(s.level == 0 ? 1 : 0, s.level == 0 ? 10.5 : 9.5);
    pdf.text_color(INK);
    double indent = 6.0 * s.level;
    pdf.x = pdf.l + indent;
    double w = pdf.W - pdf.l - pdf.r - indent - 12;
    pdf.cell(w, 6.5, uleft(s.name, 95), 'L', false, "", false, s.page);
    pdf.cell(12, 6.5, std::to_string(s.page + 1), 'R', true);
  }
  pdf.in_toc = false;
  pdf.cur = last;
  pdf.y = keep_y;

  mkdirs(out.parent_path());
  std::string data = pdf.output();
  if (!write_file(out, data)) throw std::runtime_error("no se pudo escribir " + S(out));
  return {{"path", S(out)}, {"pages", pdf.pages.size()}, {"images_embedded", embedded}, {"unicode_fonts", false}};
}

}  // namespace nx::rag
