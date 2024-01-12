#include "onvif_app_shutdown.h"
#include "../animations/dotted_slider.h"
#include "../gst/player.h"
#include "clogger.h"

void * _thread_destruction(void * event){
    C_INFO("Starting clean up thread...\n");
    OnvifApp * app = (OnvifApp *) event;
    
    //This what may take a long time.
    //Destroying the EventQueue will hang until all threads are finished
    OnvifApp__destroy(app);

    gtk_main_quit();

    pthread_exit(0);
}

void force_shutdown_cb(AppDialogEvent * event){
    C_WARN("Aborting GTK main thread!!");
    gtk_main_quit();
}

int ONVIF_APP_SHUTTING_DOWN = 0;

void onvif_app_shutdown(OnvifApp * data){
    if(!ONVIF_APP_SHUTTING_DOWN){
        C_INFO("Calling App Shutdown...\n");
        ONVIF_APP_SHUTTING_DOWN = 1;
    } else {
        C_WARN("Application already shutting down...\n");
        return;
    }
    
    GtkWidget * image = create_dotted_slider_animation(10,1);
    MsgDialog * dialog = OnvifApp__get_msg_dialog(data);
    MsgDialog__set_icon(dialog, image);
    AppDialog__set_closable((AppDialog*)dialog, 0);
    AppDialog__set_submit_label((AppDialog*)dialog, "Force Shutdown!");
    AppDialog__set_title((AppDialog*)dialog,"Shutting down...");
    AppDialog__set_cancellable((AppDialog*)dialog,0);
    MsgDialog__set_message(dialog,"Waiting for running task to finish...");
    AppDialog__show((AppDialog *) dialog,force_shutdown_cb,NULL,data);

    pthread_t pthread;
    pthread_create(&pthread, NULL, _thread_destruction, data);
    pthread_detach(pthread);
}