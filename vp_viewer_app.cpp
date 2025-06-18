// g++ main.cpp -std=c++17 `pkg-config --cflags --libs gtkmm-3.0` -o vp_viewer
#include <gtkmm.h>
#include <fstream>
#include <iostream>
#include <map>
#include <vector>
#include <stack>
#include <cstring>
#include <sstream>
#include <filesystem>

struct VPEntry {
    int offset;
    int size;
    std::string name;
    int timestamp;
    bool is_dir = false;
    std::string full_path;
};

class VPParser {
public:
    bool load(const std::string& filename) {
        std::ifstream file(filename, std::ios::binary);
        if (!file) return false;

        char header[4];
        file.read(header, 4);
        if (std::strncmp(header, "VPVP", 4) != 0) return false;

        int version, diroffset, direntries;
        file.read(reinterpret_cast<char*>(&version), sizeof(version));
        file.read(reinterpret_cast<char*>(&diroffset), sizeof(diroffset));
        file.read(reinterpret_cast<char*>(&direntries), sizeof(direntries));

        entries.clear();
        file.seekg(diroffset, std::ios::beg);

        std::stack<std::string> path_stack;
        path_stack.push(""); // root

        for (int i = 0; i < direntries; ++i) {
            VPEntry entry;
            file.read(reinterpret_cast<char*>(&entry.offset), sizeof(entry.offset));
            file.read(reinterpret_cast<char*>(&entry.size), sizeof(entry.size));

            char name[32];
            file.read(name, 32);
            entry.name = name;

            file.read(reinterpret_cast<char*>(&entry.timestamp), sizeof(entry.timestamp));

            if (entry.name == "..") {
                path_stack.pop();
                continue;
            }

            entry.is_dir = (entry.size == 0);
            std::string current_path = path_stack.top();
            if (!current_path.empty()) current_path += "/";
            current_path += entry.name;
            entry.full_path = current_path;

            if (entry.is_dir) {
                path_stack.push(current_path);
            }

            entries.push_back(entry);
        }

        this->file = std::move(file);
        return true;
    }

    std::vector<VPEntry> entries;
    std::ifstream file;
};

class VPViewerWindow : public Gtk::Window {
public:
    VPViewerWindow()
    : m_box(Gtk::ORIENTATION_VERTICAL), m_paned(Gtk::ORIENTATION_HORIZONTAL) {
        set_title("VP Viewer");
        set_default_size(800, 600);

        add(m_box);

        auto file_menu = Gtk::make_managed<Gtk::Menu>();

        auto open_item = Gtk::make_managed<Gtk::MenuItem>("Open");
        open_item->signal_activate().connect(sigc::mem_fun(*this, &VPViewerWindow::on_open_file));
        file_menu->append(*open_item);
        open_item->show();

        auto extract_item = Gtk::make_managed<Gtk::MenuItem>("Extract");
        extract_item->signal_activate().connect(sigc::mem_fun(*this, &VPViewerWindow::on_extract_file));
        file_menu->append(*extract_item);
        extract_item->show();

        auto extract_all_item = Gtk::make_managed<Gtk::MenuItem>("Extract All");
        extract_all_item->signal_activate().connect(sigc::mem_fun(*this, &VPViewerWindow::on_extract_all));
        file_menu->append(*extract_all_item);
        extract_all_item->show();

        auto quit_item = Gtk::make_managed<Gtk::MenuItem>("Quit");
        quit_item->signal_activate().connect([this]() { hide(); });
        file_menu->append(*quit_item);
        quit_item->show();

        auto menubar_item = Gtk::make_managed<Gtk::MenuItem>("File");
        menubar_item->set_submenu(*file_menu);
        m_menubar.append(*menubar_item);
        menubar_item->show();

        m_box.pack_start(m_menubar, Gtk::PACK_SHRINK);
        m_box.pack_start(m_paned);

        m_treestore = Gtk::TreeStore::create(m_columns);
        m_treeview.set_model(m_treestore);
        m_treeview.append_column("Filename", m_columns.m_col_name);
        m_treeview.get_selection()->signal_changed().connect(sigc::mem_fun(*this, &VPViewerWindow::on_tree_selection_changed));
	m_treeview_scroll.add(m_treeview);
	m_treeview_scroll.set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);

        m_paned.pack1(m_treeview_scroll);
	m_text_view.set_editable(false);
	m_text_scroll.add(m_text_view);
	m_text_scroll.set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);

	m_stack.add(m_text_scroll, "text");
	m_stack.add(m_drawing_area, "image");
	m_paned.pack2(m_stack);
        show_all_children();
    }

private:
    class ModelColumns : public Gtk::TreeModel::ColumnRecord {
    public:
        ModelColumns() { add(m_col_name); add(m_col_index); }
        Gtk::TreeModelColumn<Glib::ustring> m_col_name;
        Gtk::TreeModelColumn<int> m_col_index;
    } m_columns;

    Gtk::Box m_box;
    Gtk::MenuBar m_menubar;
    Gtk::Paned m_paned;
    Gtk::TreeView m_treeview;
    Gtk::Stack m_stack;
    Gtk::ScrolledWindow m_text_scroll;
    Gtk::ScrolledWindow m_treeview_scroll;
    Gtk::TextView m_text_view;
    Gtk::DrawingArea m_drawing_area;
    Glib::RefPtr<Gtk::TreeStore> m_treestore;
    VPParser m_parser;


    void on_open_file() {
        Gtk::FileChooserDialog dialog(*this, "Open .vp File", Gtk::FILE_CHOOSER_ACTION_OPEN);
        dialog.add_button("Cancel", Gtk::RESPONSE_CANCEL);
        dialog.add_button("Open", Gtk::RESPONSE_OK);

        if (dialog.run() == Gtk::RESPONSE_OK) {
            if (m_parser.load(dialog.get_filename())) {
                m_treestore->clear();
                std::map<std::string, Gtk::TreeModel::Row> dir_map;

                for (size_t i = 0; i < m_parser.entries.size(); ++i) {
                    const auto& entry = m_parser.entries[i];
                    std::string parent_path = entry.full_path.substr(0, entry.full_path.find_last_of("/"));
                    Gtk::TreeModel::Row parent;

                    if (!parent_path.empty() && dir_map.count(parent_path)) {
                        parent = dir_map[parent_path];
                    }

                    Gtk::TreeModel::Row row;
                    if (parent) {
                        row = *(m_treestore->append(parent.children()));
                    } else {
                        row = *(m_treestore->append());
                    }

                    row[m_columns.m_col_name] = entry.name;
                    row[m_columns.m_col_index] = static_cast<int>(i);

                    if (entry.is_dir) {
                        dir_map[entry.full_path] = row;
                    }
                }
            }
        }
    }

    void on_extract_file() {
        auto iter = m_treeview.get_selection()->get_selected();
        if (!iter) return;
        int index = (*iter)[m_columns.m_col_index];
        const auto& entry = m_parser.entries[index];

        if (entry.size > 0) {
            Gtk::FileChooserDialog dialog(*this, "Save Extracted File", Gtk::FILE_CHOOSER_ACTION_SAVE);
            dialog.set_current_name(entry.name);
            dialog.add_button("Cancel", Gtk::RESPONSE_CANCEL);
            dialog.add_button("Save", Gtk::RESPONSE_OK);

            if (dialog.run() == Gtk::RESPONSE_OK) {
                std::ofstream out(dialog.get_filename(), std::ios::binary);
                if (out) {
                    m_parser.file.seekg(entry.offset);
                    std::vector<char> buffer(entry.size);
                    m_parser.file.read(buffer.data(), entry.size);
                    out.write(buffer.data(), entry.size);
                }
            }
        }
    }

    void on_extract_all() {
        Gtk::FileChooserDialog dialog(*this, "Select Folder to Extract All Files", Gtk::FILE_CHOOSER_ACTION_SELECT_FOLDER);
        dialog.add_button("Cancel", Gtk::RESPONSE_CANCEL);
        dialog.add_button("Select", Gtk::RESPONSE_OK);

        if (dialog.run() == Gtk::RESPONSE_OK) {
            std::string base_path = dialog.get_filename();
            for (const auto& entry : m_parser.entries) {
                if (entry.size > 0) {
                    std::filesystem::path full_path = base_path + "/" + entry.full_path;
                    std::filesystem::create_directories(full_path.parent_path());
                    std::ofstream out(full_path, std::ios::binary);
                    if (out) {
                        m_parser.file.seekg(entry.offset);
                        std::vector<char> buffer(entry.size);
                        m_parser.file.read(buffer.data(), entry.size);
                        out.write(buffer.data(), entry.size);
                    }
                }
            }
        }
    }


	void on_tree_selection_changed() {
	    auto iter = m_treeview.get_selection()->get_selected();
	    if (!iter) return;
	    int index = (*iter)[m_columns.m_col_index];
	    const auto& entry = m_parser.entries[index];
	
	    if (entry.size > 0) {
	        m_parser.file.seekg(entry.offset);
	        std::vector<char> buffer(entry.size);
	        m_parser.file.read(buffer.data(), entry.size);
		
	        auto ext_pos = entry.name.find_last_of('.');
	        std::string ext = (ext_pos != std::string::npos) ? entry.name.substr(ext_pos + 1) : "";
	
	        if (ext == "txt" || ext == "tbl" || ext == "fs2") {
	            std::string content(buffer.begin(), buffer.end());
	            m_text_view.get_buffer()->set_text(content);
        	    m_stack.set_visible_child(m_text_scroll);
	        } else if (ext == "pcx") {
	            // PCX decoding not natively supported by Gdk::PixbufLoader.
	            // Placeholder: real implementation should decode to RGB buffer.
	            std::cerr << "PCX viewing not implemented yet.\n";
	            m_text_view.get_buffer()->set_text("[PCX Image Viewer Not Yet Implemented]");
	            m_stack.set_visible_child(m_text_scroll);
	        } else {
	            // Try to load as image (e.g., supported format)
	            try {
	                auto loader = Gdk::PixbufLoader::create();
	                loader->write(reinterpret_cast<const guint8*>(buffer.data()), buffer.size());
	                loader->close();
	                auto pixbuf = loader->get_pixbuf();
	
	                m_drawing_area.signal_draw().connect([pixbuf](const Cairo::RefPtr<Cairo::Context>& cr) {
	                    Gdk::Cairo::set_source_pixbuf(cr, pixbuf, 0, 0);
	                    cr->paint();
                    return true;
	                }, false);
	                m_stack.set_visible_child(m_drawing_area);
	                m_drawing_area.queue_draw();
	            } catch (...) {
	                m_text_view.get_buffer()->set_text("[Unknown binary or unsupported format]");
	                m_stack.set_visible_child(m_text_scroll);
	            }
	        }
	    }
	}
};

int main(int argc, char* argv[]) {
    auto app = Gtk::Application::create(argc, argv, "org.example.vpviewer");
    VPViewerWindow window;
    return app->run(window);
}
