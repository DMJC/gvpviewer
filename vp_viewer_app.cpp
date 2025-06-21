#include <gtkmm.h>
#include <fstream>
#include <iostream>
#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/gstclock.h>
#include <glibmm/fileutils.h>
#include <iomanip>
#include <map>
#include <vector>
#include <stack>
#include <cstring>
#include <sstream>
#include <filesystem>
#include "ani_decoder.h"
#include "pcx_decoder.h"
#include "pof_decoder.h"

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
	this->filename = filename;
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
    std::string filename;
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

		m_drawing_area.signal_draw().connect(sigc::mem_fun(*this, &VPViewerWindow::on_draw_pcx));
		m_drawing_area.signal_draw().connect(sigc::mem_fun(*this, &VPViewerWindow::on_draw_ani));

        m_paned.pack1(m_treeview_scroll);
		m_text_view.set_editable(false);
		m_text_scroll.add(m_text_view);
		m_text_scroll.set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);

		m_stack.add(m_text_scroll, "text");
		m_stack.add(m_drawing_area, "image");
//		m_stack.add(m_ani_widget, "animation");
		m_stack.add(m_grid, "wave");
		m_paned.pack2(m_stack);
        show_all_children();
    }

    bool on_draw_pcx(const Cairo::RefPtr<Cairo::Context>& cr) {
        if (m_current_pixbuf) {
            Gdk::Cairo::set_source_pixbuf(cr, m_current_pixbuf, 0, 0);
            cr->paint();
        }
        return true;
    }

	bool on_draw_ani(const Cairo::RefPtr<Cairo::Context>& cr) {
	    if (m_current_pixbuf)
	        Gdk::Cairo::set_source_pixbuf(cr, m_current_pixbuf, 0, 0), cr->paint();
	    return true;
	}

protected:
    void on_play_clicked(const VPEntry& entry) {
        if (!load_audio_data(entry))
            return;

	//Check if currently playing and then clear the pipeline
	GstState current_state;
	gst_element_get_state(m_pipeline, &current_state, nullptr, GST_CLOCK_TIME_NONE);

	if (current_state == GST_STATE_PLAYING) {
	    // Clear pipeline
	    gst_element_set_state(m_pipeline, GST_STATE_NULL);
	    gst_object_unref(m_pipeline);
	    m_pipeline = nullptr;
	}

        // Create GStreamer elements
        m_pipeline = gst_pipeline_new("vp-pipeline");
        GstElement* appsrc = gst_element_factory_make("appsrc", "source");
        GstElement* decodebin = gst_element_factory_make("decodebin", "decode");
        GstElement* convert = gst_element_factory_make("audioconvert", "convert");
        GstElement* sink = gst_element_factory_make("autoaudiosink", "sink");

        if (!m_pipeline || !appsrc || !decodebin || !convert || !sink) {
            std::cerr << "Failed to create one or more GStreamer elements." << std::endl;
            return;
        }

        // Assemble pipeline
        gst_bin_add_many(GST_BIN(m_pipeline), appsrc, decodebin, convert, sink, nullptr);
        gst_element_link(appsrc, decodebin);
        g_signal_connect(decodebin, "pad-added", G_CALLBACK(on_pad_added), convert);
        gst_element_link(convert, sink);

        // Configure appsrc
        GstAppSrc* appsrc_cast = GST_APP_SRC(appsrc);
        gst_app_src_set_stream_type(appsrc_cast, GST_APP_STREAM_TYPE_STREAM);
        gst_app_src_set_size(appsrc_cast, m_audio_data.size());

        // WAV type
        GstCaps* caps = gst_caps_new_simple("audio/x-wav", nullptr);
        gst_app_src_set_caps(appsrc_cast, caps);
        gst_caps_unref(caps);

        // Push the buffer
        GstBuffer* buffer = gst_buffer_new_allocate(nullptr, m_audio_data.size(), nullptr);
        gst_buffer_fill(buffer, 0, m_audio_data.data(), m_audio_data.size());
        gst_app_src_push_buffer(appsrc_cast, buffer);
        gst_app_src_end_of_stream(appsrc_cast);

        // Start playback
        gst_element_set_state(m_pipeline, GST_STATE_PLAYING);
    }

    void on_pause_clicked() {
        gst_element_set_state(m_pipeline, GST_STATE_PAUSED);
    }

    void on_stop_clicked() {
        gst_element_set_state(m_pipeline, GST_STATE_READY);
    }

    void on_restart_clicked() {
        gst_element_set_state(m_pipeline, GST_STATE_READY);
        gst_element_set_state(m_pipeline, GST_STATE_PLAYING);
    }

    bool load_audio_data(VPEntry entry) {
        std::ifstream file(m_parser.filename, std::ios::binary);
            std::cerr << "File: " << entry.name << std::endl;
            std::cerr << "File: " << entry.offset << std::endl;
            std::cerr << "File: " << entry.size << std::endl;

        if (!file) {
            std::cerr << "Failed to open: " << entry.name << std::endl;
            return false;
        }

        file.seekg(0, std::ios::end);
        size_t total_size = file.tellg();
        if (entry.offset + entry.size > total_size) {
            std::cerr << "Invalid VPEntry offset/size (out of bounds)." << std::endl;
            return false;
        }

        file.seekg(entry.offset);
        m_audio_data.resize(entry.size);
        file.read(reinterpret_cast<char*>(m_audio_data.data()), entry.size);

        return true;
    }

    bool on_timeout() {
        if (!GST_IS_ELEMENT(m_playbin))
            return true;

        gint64 pos = 0;
        gint64 dur = 0;

        if (gst_element_query_position(m_pipeline, GST_FORMAT_TIME, &pos) &&
            gst_element_query_duration(m_pipeline, GST_FORMAT_TIME, &dur)) {
            double current_sec = (double)pos / GST_SECOND;
            double total_sec = (double)dur / GST_SECOND;

            m_adjustment->set_upper(total_sec);
            m_adjustment->set_value(current_sec);
        }

        return true; // keep timer
    }

    static void on_pad_added(GstElement* src, GstPad* pad, gpointer data) {
        GstElement* convert = static_cast<GstElement*>(data);
        GstPad* sinkpad = gst_element_get_static_pad(convert, "sink");
        if (gst_pad_link(pad, sinkpad) != GST_PAD_LINK_OK)
            std::cerr << "Failed to link decodebin to audioconvert" << std::endl;
        gst_object_unref(sinkpad);
    }
//    VPEntry m_entry;
    GstElement* m_pipeline = nullptr;
    std::vector<uint8_t> m_audio_data;

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
    Gtk::Grid m_grid;
    Gtk::Label m_label;
    Gtk::Scrollbar m_scrollbar;
    Gtk::Button m_button_play, m_button_pause, m_button_stop, m_button_restart;
    GstElement* m_playbin = nullptr;
//    ANIMovie ani;
    PCXImage pcx;
    bool m_uri_set = false;
    Glib::ustring filename_only;
    std::string title;
    Glib::RefPtr<Gtk::Adjustment> m_adjustment;
    Glib::RefPtr<Gtk::TreeStore> m_treestore;
    Glib::RefPtr<Gdk::Pixbuf> m_current_pixbuf;
    VPParser m_parser;

    void on_open_file() {
        Gtk::FileChooserDialog dialog(*this, "Open .vp File", Gtk::FILE_CHOOSER_ACTION_OPEN);
        dialog.add_button("Cancel", Gtk::RESPONSE_CANCEL);
        dialog.add_button("Open", Gtk::RESPONSE_OK);

	auto filter_vp = Gtk::FileFilter::create();
        filter_vp->set_name("VP files");
        filter_vp->add_pattern("*vp");
        dialog.add_filter(filter_vp);
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
        std::string full_path = dialog.get_filename();
        // Extract just the filename from the full path
	filename_only = Glib::filename_display_basename(full_path);
	title = "VP Viewer - " + filename_only;
        set_title(title);
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
	
	        if (ext == "ani" || ext == "ANI") {
				std::vector<uint8_t> byte_buffer(buffer.begin(), buffer.end());
/*				ani.load_ani_from_memory(byte_buffer);
				m_current_pixbuf = .play();
				m_stack.set_visible_child(m_ani_widget);
				m_ani_widget.queue_draw();*/
	                std::cerr << "Ani not implemented yet." << std::endl;
			} else if (ext == "txt" || ext == "hcf" || ext == "tbl" || ext == "fs2" || ext == "fc2" || ext == "TXT" || ext == "HCF" || ext == "TBL" || ext == "FS2" || ext == "FC2") {
	            std::string content(buffer.begin(), buffer.end());
	            m_text_view.get_buffer()->set_text(content);
        	    m_stack.set_visible_child(m_text_scroll);
	        } else if (ext == "pcx" || ext == "PCX") {
				std::vector<uint8_t> byte_buffer(buffer.begin(), buffer.end());
				pcx = load_pcx_from_memory(byte_buffer);
				m_current_pixbuf = Gdk::Pixbuf::create_from_data(
				    pcx.rgba_data.data(),
				    Gdk::COLORSPACE_RGB,
				    true,
				    8,
				    pcx.width,
				    pcx.height,
				    pcx.width * 4
				);
				m_stack.set_visible_child(m_drawing_area);
				m_drawing_area.queue_draw();
	        } else if (ext == "pof" || ext == "POF") {
	                std::cerr << "POF 3D model viewer not implemented yet." << std::endl;
	        } else if (ext == "wav") {
	            gst_init(nullptr, nullptr);
	            m_playbin = gst_element_factory_make("playbin", "player");
	            if (!m_playbin) {
	                std::cerr << "Failed to create playbin element." << std::endl;
	                std::exit(1);
	            }
            	    // Label (spans all 4 columns)
	            m_label.set_text("Filename:");
	     	    m_label.set_text(entry.name);
	            m_grid.attach(m_label, 0, 0, 4, 1); // column, row, width, height

	            // Horizontal scrollbar (also 4 columns wide)
	            m_scrollbar.set_orientation(Gtk::ORIENTATION_HORIZONTAL);
	            m_adjustment = Gtk::Adjustment::create(0, 0, 100, 1, 10);
	            m_scrollbar.set_adjustment(m_adjustment);
        	    m_grid.attach(m_scrollbar, 0, 1, 4, 1);


		    m_button_play.signal_clicked().connect([this, &entry] {
			on_play_clicked(entry);
		    });
//	            m_button_play.signal_clicked().connect(sigc::mem_fun(*this, &VPViewerWindow::on_play_clicked));
	            m_button_pause.signal_clicked().connect(sigc::mem_fun(*this, &VPViewerWindow::on_pause_clicked));
	            m_button_stop.signal_clicked().connect(sigc::mem_fun(*this, &VPViewerWindow::on_stop_clicked));
	            m_button_restart.signal_clicked().connect(sigc::mem_fun(*this, &VPViewerWindow::on_restart_clicked));
	            Glib::signal_timeout().connect(sigc::mem_fun(*this, &VPViewerWindow::on_timeout), 100); 
	            // Buttons
	            m_grid.attach(m_button_play, 0, 2, 1, 1);
	            m_button_play.set_label("Play");

	            m_grid.attach(m_button_pause, 1, 2, 1, 1);
	            m_button_pause.set_label("Pause");

	            m_grid.attach(m_button_stop, 2, 2, 1, 1);
	            m_button_stop.set_label("Stop");

	            m_grid.attach(m_button_restart, 3, 2, 1, 1);
        	    m_button_restart.set_label("Restart");
		    	m_grid.show_all_children();
	            m_stack.set_visible_child(m_grid);
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
