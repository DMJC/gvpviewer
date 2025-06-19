build:
	g++ vp_viewer_app.cpp -std=c++17 `pkg-config --cflags --libs gtkmm-3.0 gstreamer-1.0 gstreamer-app-1.0` -o vpview
clean:
	rm vpview
