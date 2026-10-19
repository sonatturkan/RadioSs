# Derleme desteği

CMake yardımcı dosyalarına ayrılmış dizindir. Ana `../CMakeLists.txt`, C11 core/uygulama/test hedeflerini ve GTK/GLib/Fontconfig/JSON-GLib/libcurl bağımlılıklarını kurar. Mevcut tasarım CSS dosyaları build sırasında `tools/css_to_gtk.py` ile C içine gömülür. Derleme çıktıları `build/` altında tutulur; ekransız testlerde `RADIOS_GUI_TESTS=OFF` kullanılır.
