#include "onvif_app.h"
#include "../queue/event_queue.h"
#include "dialog/credentials_input.h"
#include "dialog/add_device.h"
#include "dialog/profiles_dialog.h"
#include "onvif_details.h"
#include "onvif_nvt.h"
#include "settings/app_settings.h"
#include "task_manager.h"
#include "clogger.h"
#include "gui_utils.h"
#include "gtkstyledimage.h"
#include "discoverer.h"
#include "onvif_app_shutdown.h"

extern char _binary_tower_png_size[];
extern char _binary_tower_png_start[];
extern char _binary_tower_png_end[];

enum {
    DEVICE_CHANGED,
    LAST_SIGNAL
};

typedef struct {
    OnvifMgrDeviceRow * device;

    int owned;

    GtkWidget *btn_scan;
    GtkWidget *window;
    GtkWidget *listbox;
    GtkWidget *main_notebook;
    GtkWidget *player_loading_handle;
    GtkWidget *task_label;

    ProfilesDialog * profiles_dialog;
    CredentialsDialog * cred_dialog;
    AddDeviceDialog * add_dialog;
    MsgDialog * msg_dialog;

    int current_page;

    OnvifDetails * details;
    AppSettings * settings;
    TaskMgr * taskmgr;

    EventQueue * queue;
    GstRtspPlayer * player;
} OnvifAppPrivate;

static guint signals[LAST_SIGNAL] = { 0 };

static void OnvifApp__ownable_interface_init (COwnableObjectInterface *iface);

G_DEFINE_TYPE_WITH_CODE(OnvifApp, OnvifApp_, G_TYPE_OBJECT, G_ADD_PRIVATE (OnvifApp)
                                    G_IMPLEMENT_INTERFACE (COWNABLE_TYPE_OBJECT,OnvifApp__ownable_interface_init))

static void OnvifApp__profile_changed_cb (OnvifMgrDeviceRow *device);
void OnvifApp__cred_dialog_login_cb(AppDialogEvent * event);
void OnvifApp__cred_dialog_cancel_cb(AppDialogEvent * event);
static gboolean * OnvifApp__discovery_finished_cb (OnvifApp * self);
static gboolean * OnvifApp__disocvery_found_server_cb (DiscoveryEvent * event);
static int OnvifApp__device_already_exist(OnvifApp * app, char * xaddr);
static void OnvifApp__add_device(OnvifApp * app, OnvifMgrDeviceRow * omgr_device);
static void OnvifApp__select_device(OnvifApp * app,  GtkListBoxRow * row);
static int OnvifApp__reload_device(OnvifMgrDeviceRow * device);
static void OnvifApp__display_device(OnvifApp * self, OnvifMgrDeviceRow * device);
static void OnvifApp__update_pages(OnvifApp * self, OnvifMgrDeviceRow * device);

gboolean * idle_select_device(void * user_data){
    OnvifMgrDeviceRow * device = ONVIFMGR_DEVICEROW(user_data);
    if(ONVIFMGR_DEVICEROWROW_HAS_OWNER(device) && gtk_list_box_row_is_selected(GTK_LIST_BOX_ROW(device))){
        OnvifApp__select_device(OnvifMgrDeviceRow__get_app(device),GTK_LIST_BOX_ROW(device));
    }
    g_object_unref(device);
    return FALSE;
}

gboolean * idle_update_pages(void * user_data){
    OnvifMgrDeviceRow * device = ONVIFMGR_DEVICEROW(user_data);
    OnvifApp * app = OnvifMgrDeviceRow__get_app(device);
    OnvifApp__update_pages(app,device);
    g_object_unref(device);
    return FALSE;
}

gboolean * idle_show_credentialsdialog (void * user_data){
    OnvifMgrDeviceRow * device = ONVIFMGR_DEVICEROW(user_data);
    OnvifAppPrivate *priv = OnvifApp__get_instance_private (OnvifMgrDeviceRow__get_app(device));
    AppDialog__show((AppDialog *) priv->cred_dialog,OnvifApp__cred_dialog_login_cb, OnvifApp__cred_dialog_cancel_cb,device);
    return FALSE;
}

gboolean * idle_hide_dialog (void * user_data){
    AppDialog * dialog = (AppDialog *) user_data;
    AppDialog__hide(dialog);
    return FALSE;
}

gboolean * idle_hide_dialog_loading (void * user_data){
    AppDialog * dialog = (AppDialog *) user_data;
    AppDialog__hide_loading(dialog);
    return FALSE;
}

gboolean * idle_add_device(void * user_data){
    OnvifMgrDeviceRow * dev = ONVIFMGR_DEVICEROW(user_data);
    OnvifApp__add_device(OnvifMgrDeviceRow__get_app(dev),dev);
    return FALSE;
}

void _player_retry_stream(void * user_data){
    C_TRACE("_player_retry_stream");
    OnvifMgrDeviceRow * device = ONVIFMGR_DEVICEROW(user_data);
    OnvifApp * self = OnvifMgrDeviceRow__get_app(device);
    OnvifAppPrivate *priv = OnvifApp__get_instance_private (self);

    //Check if the device is valid and selected
    if(ONVIFMGR_DEVICEROWROW_HAS_OWNER(device) && OnvifMgrDeviceRow__is_selected(device)){
        sleep(2);//Wait to avoid spamming the camera
        //Check again if the device is valid and selected after waiting
        if(ONVIFMGR_DEVICEROWROW_HAS_OWNER(device) && OnvifMgrDeviceRow__is_selected(device)){
            GstRtspPlayer__retry(priv->player);
        }
    }
    g_object_unref(device);
}

void _dicovery_found_server_cb (DiscoveryEvent * event) {
    C_TRACE("_dicovery_found_server_cb");
    OnvifApp * app = (OnvifApp *) event->data;
    if(!COwnableObject__has_owner(COWNABLE_OBJECT(app))){
        return;
    }
    
    gdk_threads_add_idle(G_SOURCE_FUNC(OnvifApp__disocvery_found_server_cb),event);
}

void _start_onvif_discovery(void * user_data){
    C_TRACE("_start_onvif_discovery");
    OnvifApp * self = (OnvifApp *) user_data;
    if(!COwnableObject__has_owner(COWNABLE_OBJECT(self))){
        g_object_unref(self);
        return;
    }
    
    OnvifAppPrivate *priv = OnvifApp__get_instance_private (self);
    //Start UDP Scan
    struct UdpDiscoverer discoverer = UdpDiscoverer__create(_dicovery_found_server_cb);
    UdpDiscoverer__start(&discoverer, self, AppSettingsDiscovery__get_repeat(priv->settings->discovery), AppSettingsDiscovery__get_timeout(priv->settings->discovery));
    
    gdk_threads_add_idle(G_SOURCE_FUNC(OnvifApp__discovery_finished_cb),self);
}

void _display_onvif_device(void * user_data){
    C_TRACE("_display_onvif_device");
    OnvifMgrDeviceRow * omgr_device = (OnvifMgrDeviceRow *) user_data;

    if(!ONVIFMGR_DEVICEROWROW_HAS_OWNER(omgr_device)){
        C_TRAIL("_display_onvif_device - invalid device.");
        goto exit;
    }
    
    OnvifDevice * odev = OnvifMgrDeviceRow__get_device(omgr_device);

    /* Authentication check */
    OnvifDevice__authenticate(odev);
        

    if(!ONVIFMGR_DEVICEROWROW_HAS_OWNER(omgr_device)){
        C_TRAIL("_display_onvif_device - invalid device.");
        goto exit;
    } else if (OnvifDevice__get_last_error(odev) == ONVIF_ERROR_NOT_AUTHORIZED){
        goto updatethumb;
    }

    /* Display Profile dropdown */
    if(!OnvifMgrDeviceRow__get_profile(omgr_device)){
        OnvifProfiles * profiles = OnvifMediaService__get_profiles(OnvifDevice__get_media_service(odev));
        OnvifMgrDeviceRow__set_profile(omgr_device,OnvifProfiles__get_profile(profiles,0));
        OnvifProfiles__destroy(profiles);
        //We don't care for the initial profile event since the default index is 0.
        //Connecting to signal only after setting the default profile
        g_signal_connect (G_OBJECT (omgr_device), "profile-changed", G_CALLBACK (OnvifApp__profile_changed_cb), NULL);

    }

    /* Display row thumbnail. Default to profile index 0 */
updatethumb:
    OnvifMgrDeviceRow__load_thumbnail(omgr_device);
    if(!OnvifMgrDeviceRow__is_initialized(omgr_device)){ //First time initialization
        OnvifMgrDeviceRow__set_initialized(omgr_device);
        //If it was selected while initializing, redispatch select_device
        if(ONVIFMGR_DEVICEROWROW_HAS_OWNER(omgr_device) && gtk_list_box_row_is_selected(GTK_LIST_BOX_ROW(omgr_device))){
            g_object_ref(omgr_device);
            gdk_threads_add_idle(G_SOURCE_FUNC(idle_select_device),omgr_device);
        }
    }
exit:
    g_object_unref(omgr_device);
}

void _profile_callback (void * user_data){
    C_TRACE("_profile_callback");
    OnvifMgrDeviceRow * device = ONVIFMGR_DEVICEROW(user_data);
    if(!ONVIFMGR_DEVICEROWROW_HAS_OWNER(device)){
        C_TRAIL("_profile_callback - invalid device.");
        return;
    }
    
    if(OnvifMgrDeviceRow__is_selected(device)){
        OnvifAppPrivate *priv = OnvifApp__get_instance_private (OnvifMgrDeviceRow__get_app(device));
        GstRtspPlayer__stop(priv->player);
    }
    OnvifApp__reload_device(device);

    g_object_unref(device);
}

void _play_onvif_stream(void * user_data){
    ONVIFMGR_DEVICEROW_TRACE("_play_onvif_stream %s",user_data);
    OnvifMgrDeviceRow * device = ONVIFMGR_DEVICEROW(user_data);

    //Check if device is still valid. (User performed scan before thread started)
    if(!ONVIFMGR_DEVICEROWROW_HAS_OWNER(device) || !OnvifMgrDeviceRow__is_selected(device)){
        C_TRAIL("_play_onvif_stream - invalid device.");
        goto exit;
    }

    OnvifDevice * odev = OnvifMgrDeviceRow__get_device(device);
    OnvifApp * app = OnvifMgrDeviceRow__get_app(device);
    OnvifAppPrivate *priv = OnvifApp__get_instance_private (app);
    
    /* Authentication check */
    OnvifDevice__authenticate(odev);

    if(!ONVIFMGR_DEVICEROWROW_HAS_OWNER(device)) {
        C_TRAIL("_play_onvif_stream - invalid device.");
        goto exit;
    }
    if(OnvifDevice__get_last_error(odev) != ONVIF_ERROR_NONE && OnvifMgrDeviceRow__is_selected(device)){
        goto exit;
    }

    /* Set the URI to play */
    char * uri = OnvifMediaService__getStreamUri(OnvifDevice__get_media_service(odev),OnvifProfile__get_index(OnvifMgrDeviceRow__get_profile(device)));
    
    if(ONVIFMGR_DEVICEROWROW_HAS_OWNER(device) && OnvifDevice__get_last_error(odev) == ONVIF_ERROR_NONE){
        GstRtspPlayer__set_playback_url(priv->player,uri);
        char * port = OnvifDevice__get_port(OnvifMgrDeviceRow__get_device(device));
        GstRtspPlayer__set_port_fallback(priv->player,port);
        free(port);

        char * host = OnvifDevice__get_host(OnvifMgrDeviceRow__get_device(device));
        GstRtspPlayer__set_host_fallback(priv->player,host);
        free(host);
        
        OnvifCredentials * ocreds = OnvifDevice__get_credentials(odev);
        char * user = OnvifCredentials__get_username(ocreds);
        char * pass = OnvifCredentials__get_password(ocreds);
        GstRtspPlayer__set_credentials(priv->player, user, pass);
        free(user);
        free(pass);

        GstRtspPlayer__play(priv->player);
    } else if(!ONVIFMGR_DEVICEROWROW_HAS_OWNER(device)) {
        C_TRAIL("_play_onvif_stream - invalid device.");
    }
    free(uri);

exit:
    C_TRACE("_play_onvif_stream - done\n");
    g_object_unref(device);
}

void _stop_onvif_stream(void * user_data){
    C_TRACE("_stop_onvif_stream");
    OnvifApp * app = (OnvifApp *) user_data;
    if(!COwnableObject__has_owner(COWNABLE_OBJECT(app))){
        goto exit;
    }

    OnvifAppPrivate *priv = OnvifApp__get_instance_private (app);
    GstRtspPlayer__stop(priv->player);

exit:
    g_object_unref(app);
}

void _onvif_authentication_reload(void * user_data){
    AppDialogEvent * event = (AppDialogEvent *) user_data;
    OnvifMgrDeviceRow * device = ONVIFMGR_DEVICEROW(event->user_data);

    C_DEBUG("_onvif_authentication_reload\n");
    //Check device is still valid before adding ref (User performed scan before thread started)
    if(!ONVIFMGR_DEVICEROWROW_HAS_OWNER(device)){
        C_TRAIL("_onvif_authentication_reload - invalid device\n");
        free(event);
        return;
    }

    OnvifAppPrivate *priv = OnvifApp__get_instance_private (OnvifMgrDeviceRow__get_app(device));
    if(OnvifApp__reload_device(device)){
        gdk_threads_add_idle(G_SOURCE_FUNC(idle_hide_dialog),priv->cred_dialog);
        g_object_unref(device);
    } else {
        gdk_threads_add_idle(G_SOURCE_FUNC(idle_hide_dialog_loading),priv->cred_dialog);
    }
    free(event);
}

void _onvif_device_add(void * user_data){
    AppDialogEvent * event = (AppDialogEvent *) user_data;
    AddDeviceDialog * dialog = (AddDeviceDialog *) event->dialog;
    const char * host = AddDeviceDialog__get_host((AddDeviceDialog *)event->dialog);
    const char * port = AddDeviceDialog__get_port((AddDeviceDialog *)event->dialog);
    const char * protocol = AddDeviceDialog__get_protocol((AddDeviceDialog *)event->dialog);

    char fullurl[strlen(protocol) + 3 + strlen(host) + 1 + strlen(port) + strlen("/onvif/device_service") + 1];
    strcpy(fullurl,protocol);
    strcat(fullurl,"://");
    strcat(fullurl,host);
    strcat(fullurl,":");
    strcat(fullurl,port);
    strcat(fullurl,"/onvif/device_service");

    C_INFO("Manually adding URL : '%s'",fullurl);

    OnvifDevice * onvif_dev = OnvifDevice__create((char *)fullurl);
    if(!OnvifDevice__is_valid(onvif_dev)){
        C_ERROR("Invalid URL provided\n");
        OnvifDevice__destroy(onvif_dev);
        goto exit;
    }
    OnvifDevice__set_credentials(onvif_dev,AddDeviceDialog__get_user((AddDeviceDialog *)event->dialog),AddDeviceDialog__get_pass((AddDeviceDialog *)event->dialog));
   
    /* Authentication check */
    OnvifDevice__authenticate(onvif_dev);

    OnvifErrorTypes oerror = OnvifDevice__get_last_error(onvif_dev);
    if(oerror == ONVIF_ERROR_NONE) {
        //Extract scope
        OnvifDeviceService * devserv = OnvifDevice__get_device_service(onvif_dev);
        OnvifScopes * scopes = OnvifDeviceService__getScopes(devserv);

        char * name = OnvifScopes__extract_scope(scopes,"name");
        char * hardware = OnvifScopes__extract_scope(scopes,"hardware");
        char * location = OnvifScopes__extract_scope(scopes,"location");

        GtkWidget * omgr_device = OnvifMgrDeviceRow__new((OnvifApp *) event->user_data, onvif_dev,name,hardware,location);
        
        gdk_threads_add_idle(G_SOURCE_FUNC(idle_add_device),omgr_device);

        free(name);
        free(hardware);
        free(location);
        //TODO Save manually added camera to settings

        OnvifScopes__destroy(scopes);
    } else {
        if(oerror == ONVIF_ERROR_NOT_AUTHORIZED){
            AddDeviceDialog__set_error(dialog,"Unauthorized...");
        } else if(oerror == ONVIF_ERROR_NOT_VALID){
            AddDeviceDialog__set_error(dialog,"Not a valid ONVIF device...");
        } else if(oerror == ONVIF_ERROR_SOAP){
            AddDeviceDialog__set_error(dialog,"Unexected soap error occured...");
        } else if(oerror == ONVIF_ERROR_CONNECTION){
            AddDeviceDialog__set_error(dialog,"Failed to connect...");
        } else {
            C_ERROR("An soap error was encountered %d\n",oerror);
        }
        OnvifDevice__destroy(onvif_dev);
        goto exit;
    }

    gdk_threads_add_idle(G_SOURCE_FUNC(idle_hide_dialog),dialog);
    goto free;
exit:
    gdk_threads_add_idle(G_SOURCE_FUNC(idle_hide_dialog_loading),dialog);
free:
    free(event);
}


static gboolean * OnvifApp__disocvery_found_server_cb (DiscoveryEvent * event) {
    C_TRACE("OnvifApp__found_server_cb");
    DiscoveredServer * server = event->server;
    OnvifApp * app = (OnvifApp *) event->data;
    if(!COwnableObject__has_owner(COWNABLE_OBJECT(app))){
        goto exit;
    }

    ProbMatch * m;
    int i;
  
    C_INFO("Found server - Match count : %i\n",server->matches->match_count);
    for (i = 0 ; i < server->matches->match_count ; ++i) {
        m = server->matches->matches[i];
        if(!OnvifApp__device_already_exist(app,m->addrs[0])){
            gchar * addr_dup = g_strdup(m->addrs[0]);
            OnvifDevice * onvif_dev = OnvifDevice__create(addr_dup);
            free(addr_dup);
            
            char * name = onvif_extract_scope("name",m);
            char * hardware = onvif_extract_scope("hardware",m);
            char * location = onvif_extract_scope("location",m);
            
            GtkWidget * omgr_device = OnvifMgrDeviceRow__new(app,onvif_dev,name,hardware,location);
            OnvifApp__add_device(app,ONVIFMGR_DEVICEROW(omgr_device));
            free(name);
            free(hardware);
            free(location);
        }
    }

exit:
    CObject__destroy((CObject*)event);
    return FALSE;
}

void OnvifApp__cred_dialog_login_cb(AppDialogEvent * event){
    C_INFO("OnvifAuthentication attempt...\n");
    OnvifMgrDeviceRow * device = ONVIFMGR_DEVICEROW(event->user_data);
    OnvifAppPrivate *priv = OnvifApp__get_instance_private (OnvifMgrDeviceRow__get_app(device));
    AppDialog__show_loading((AppDialog*)priv->cred_dialog, "ONVIF Authentication attempt...");
    OnvifDevice__set_credentials(OnvifMgrDeviceRow__get_device(device),CredentialsDialog__get_username((CredentialsDialog*)event->dialog),CredentialsDialog__get_password((CredentialsDialog*)event->dialog));
    EventQueue__insert(priv->queue,_onvif_authentication_reload,AppDialogEvent_copy(event));
}

void OnvifApp__cred_dialog_cancel_cb(AppDialogEvent * event){
    C_INFO("OnvifAuthentication cancelled...\n");
    g_object_unref(event->user_data);
}

void OnvifApp__player_retry_cb(GstRtspPlayer * player, void * user_data){
    C_TRACE("OnvifApp__player_retry_cb");
    OnvifApp * self = (OnvifApp *) user_data;
    OnvifAppPrivate *priv = OnvifApp__get_instance_private (self);
    g_object_ref(priv->device);
    EventQueue__insert(priv->queue,_player_retry_stream,priv->device);
}

void OnvifApp__player_error_cb(GstRtspPlayer * player, void * user_data){
    C_ERROR("Stream encountered an error\n");
    OnvifApp * self = (OnvifApp *) user_data;
    OnvifAppPrivate *priv = OnvifApp__get_instance_private (self);
    //On shutdown, the player may dispatch this event after the window is destroyed
    if(GTK_IS_SPINNER(priv->player_loading_handle)){
        gtk_spinner_stop (GTK_SPINNER (priv->player_loading_handle));
    }
}

void OnvifApp__player_stopped_cb(GstRtspPlayer * player, void * user_data){
    C_INFO("Stream stopped\n");
    //TODO Show placeholder on canvas
}

void OnvifApp__player_started_cb(GstRtspPlayer * player, void * user_data){
    C_INFO("Stream playing\n");
    OnvifApp * self = (OnvifApp *) user_data;
    OnvifAppPrivate *priv = OnvifApp__get_instance_private (self);
    if(GTK_IS_SPINNER(priv->player_loading_handle)){
        gtk_spinner_stop (GTK_SPINNER (priv->player_loading_handle));
    }
}

void OnvifApp__eq_dispatch_cb(QueueThread * thread, EventQueueType type, void * user_data){
    OnvifApp * self = (OnvifApp *) user_data;
    OnvifAppPrivate *priv = OnvifApp__get_instance_private (self);

    if(!GTK_IS_WIDGET(priv->task_label)){
        return;
    }
    EventQueue * queue = QueueThread__get_queue(thread);
    int running = EventQueue__get_running_event_count(queue);
    int pending = EventQueue__get_pending_event_count(queue);
    int total = EventQueue__get_thread_count(queue);

    char str[10];
    memset(&str,'\0',sizeof(str));
    sprintf(str, "[%d/%d]", running + pending,total);

    gui_set_label_text(priv->task_label,str);
}

void OnvifApp__setting_overscale_cb(AppSettingsStream * settings, int allow_overscale, void * user_data){
    OnvifApp * app = (OnvifApp *) user_data;
    OnvifAppPrivate *priv = OnvifApp__get_instance_private (app);
    GstRtspPlayer__set_allow_overscale(priv->player,allow_overscale);
}

void OnvifApp__profile_selected_cb(ProfilesDialog * dialog, OnvifProfile * profile){
    OnvifMgrDeviceRow * device =  ProfilesDialog__get_device(dialog);
    OnvifMgrDeviceRow__set_profile(device,profile);
}

/* This function is called when the STOP button is clicked */
static void OnvifApp__quit_cb (GtkButton *button, OnvifApp *data) {
    onvif_app_shutdown(data);
}

/* This function is called when the main window is closed */
static void OnvifApp__window_delete_event_cb (GtkWidget *widget, GdkEvent *event, OnvifApp *data) {
    onvif_app_shutdown(data);
}

void OnvifApp__btn_scan_cb (GtkWidget *widget, OnvifApp * app) {
    C_INFO("Starting ONVIF Devices Network Discovery...");
    OnvifAppPrivate *priv = OnvifApp__get_instance_private (app);

    gtk_widget_set_sensitive(widget,FALSE);

    //Clearing the list
    gtk_container_foreach (GTK_CONTAINER (priv->listbox), (GtkCallback)gui_container_remove, priv->listbox);
    // EventQueue__clear(app->queue); //Unable to safely clear because pointers are released inside pending events

    //Multiple dispatch in case of packet dropped
    g_object_ref(app);
    EventQueue__insert(priv->queue,_start_onvif_discovery,app);
}

static void OnvifApp__profile_picker_cb (OnvifMgrDeviceRow *device){
    OnvifAppPrivate *priv = OnvifApp__get_instance_private (OnvifMgrDeviceRow__get_app(device));
    ProfilesDialog__set_device(priv->profiles_dialog,device);
    AppDialog__show((AppDialog *) priv->profiles_dialog,NULL,NULL,NULL);
}

static void OnvifApp__profile_changed_cb (OnvifMgrDeviceRow *device){
    OnvifAppPrivate *priv = OnvifApp__get_instance_private (OnvifMgrDeviceRow__get_app(device));
    g_object_ref(device);
    EventQueue__insert(priv->queue,_profile_callback,device);
}

void OnvifApp__add_device_cb(AppDialogEvent * event){
    OnvifApp * app = (OnvifApp *) event->user_data;
    const char * host = AddDeviceDialog__get_host((AddDeviceDialog *)event->dialog);
    if(!host || strlen(host) < 2){
        C_WARN("Host field invalid. (too short)");
        return;
    }

    AppDialog__show_loading((AppDialog*)event->dialog, "Testing ONVIF device configuration...");
    OnvifAppPrivate *priv = OnvifApp__get_instance_private (app);
    EventQueue__insert(priv->queue,_onvif_device_add,AppDialogEvent_copy(event));
}

void OnvifApp__add_btn_cb (GtkWidget *widget, OnvifApp * app) {
    OnvifAppPrivate *priv = OnvifApp__get_instance_private (app);
    AppDialog__show((AppDialog *) priv->add_dialog,OnvifApp__add_device_cb,NULL,app);
}

void OnvifApp__row_selected_cb (GtkWidget *widget,   GtkListBoxRow* row, OnvifApp* app){
    OnvifApp__select_device(app,row);
}

static gboolean * OnvifApp__discovery_finished_cb (OnvifApp * self) {
    C_TRACE("OnvifApp__discovery_finished_cb");
    if(!COwnableObject__has_owner(COWNABLE_OBJECT(self))){
        goto exit;
    }

    OnvifAppPrivate *priv = OnvifApp__get_instance_private (self);

    if(GTK_IS_WIDGET(priv->btn_scan)){
        gtk_widget_set_sensitive(priv->btn_scan,TRUE);
    }

exit:
    g_object_unref(self);
    return FALSE;
}

//On the odd chance the by the time update_pages is invoked, the selection changed
//We use device as input parameters carried through the idle dispatch. Not from the app pointer
static void OnvifApp__update_pages(OnvifApp * self, OnvifMgrDeviceRow * device){
    OnvifAppPrivate *priv = OnvifApp__get_instance_private (self);
    //#0 NVT page already loaded
    //#2 Settings page already loaded
    //Details page is dynamically loaded
    if(priv->current_page == 1){
        OnvifDetails__clear_details(priv->details);
        OnvifDetails__update_details(priv->details,device);
    }
}

static void OnvifApp__switch_page (GtkNotebook* self, GtkWidget* page, guint page_num, OnvifApp * app) {
    C_DEBUG("OnvifApp__switch_page");
    OnvifAppPrivate *priv = OnvifApp__get_instance_private (app);
    priv->current_page = page_num;
    OnvifApp__update_pages(app, priv->device);
}

static int OnvifApp__reload_device(OnvifMgrDeviceRow * device){
    OnvifDevice * odev = OnvifMgrDeviceRow__get_device(device);

    /* Authentication check */
    OnvifDevice__authenticate(odev);
    
    //Check if device is valid and authorized (User performed scan before auth finished)
    if(!ONVIFMGR_DEVICEROWROW_HAS_OWNER(device)) {
        C_TRAIL("OnvifApp__reload_device - invalid device.");
        return 0;
    }

    if(OnvifDevice__get_last_error(odev) == ONVIF_ERROR_NOT_AUTHORIZED){
        return 0;
    }

    //Replace locked image with spinner
    GtkWidget * image = gtk_spinner_new ();
    OnvifMgrDeviceRow__set_thumbnail(device,image);

    OnvifApp * app = OnvifMgrDeviceRow__get_app(device);
    OnvifApp__display_device(app, device);

    g_object_ref(device);
    gdk_threads_add_idle(G_SOURCE_FUNC(idle_update_pages),device);
    if(OnvifDevice__get_last_error(odev) == ONVIF_ERROR_NONE && OnvifMgrDeviceRow__is_selected(device)){
        OnvifAppPrivate *priv = OnvifApp__get_instance_private (app);
        safely_start_spinner(priv->player_loading_handle);
        g_object_ref(device);
        _play_onvif_stream(device);
    }

    return 1;
}

gboolean OnvifApp__set_device(OnvifApp * app, GtkListBoxRow * row){
    OnvifAppPrivate *priv = OnvifApp__get_instance_private (app);
    if(!ONVIFMGR_IS_DEVICEROW(row)) {
        C_DEBUG("OnvifApp__set_device - NULL");
        if(ONVIFMGR_IS_DEVICEROW(priv->device)) g_object_unref(priv->device);
        priv->device = NULL;
        return FALSE; 
    }

    OnvifMgrDeviceRow * old = priv->device;

    C_DEBUG("OnvifApp__set_device - valid device");
    priv->device = ONVIFMGR_DEVICEROW(row);

    //If changing device, trade reference
    if(ONVIFMGR_DEVICEROW(old) != priv->device) {
        if(G_IS_OBJECT(old))
            g_object_unref(old);
        g_object_ref(priv->device);
    }
    return TRUE;
}

static void OnvifApp__select_device(OnvifApp * app,  GtkListBoxRow * row){
    C_DEBUG("OnvifApp__select_device");
    OnvifAppPrivate *priv = OnvifApp__get_instance_private (app);

    //Stop previous stream
    g_object_ref(app);
    EventQueue__insert(priv->queue,_stop_onvif_stream,app);

    //Clear details
    OnvifDetails__clear_details(priv->details);

    OnvifApp__set_device(app,row);

    if(!OnvifApp__set_device(app,row)){
        //In case the previous stream was in a retry cycle, force hide loading
        gtk_spinner_stop (GTK_SPINNER (priv->player_loading_handle));
    } else if(OnvifMgrDeviceRow__is_initialized(priv->device)){
        OnvifDevice * odev = OnvifMgrDeviceRow__get_device(priv->device);
        //Prompt for authentication
        if(OnvifDevice__get_last_error(odev) == ONVIF_ERROR_NOT_AUTHORIZED){
            //In case the previous stream was in a retry cycle, force hide loading
            gtk_spinner_stop (GTK_SPINNER (priv->player_loading_handle));
            //Re-dispatched to allow proper focus handling
            g_object_ref(priv->device);
            gdk_threads_add_idle(G_SOURCE_FUNC(idle_show_credentialsdialog),priv->device);
            return;
        }

        gtk_spinner_start (GTK_SPINNER (priv->player_loading_handle));
        g_object_ref(priv->device);
        EventQueue__insert(priv->queue,_play_onvif_stream,priv->device);
        OnvifApp__update_pages(app, priv->device);
    }
}

static void OnvifApp__display_device(OnvifApp * self, OnvifMgrDeviceRow * device){
    OnvifAppPrivate *priv = OnvifApp__get_instance_private (self);
    g_object_ref(device);
    EventQueue__insert(priv->queue,_display_onvif_device,device);
}

static void OnvifApp__add_device(OnvifApp * app, OnvifMgrDeviceRow * omgr_device){
    OnvifAppPrivate *priv = OnvifApp__get_instance_private (app);
    g_signal_connect (G_OBJECT (omgr_device), "profile-clicked", G_CALLBACK (OnvifApp__profile_picker_cb), NULL);

    gtk_list_box_insert (GTK_LIST_BOX (priv->listbox), GTK_WIDGET(omgr_device), -1);
    gtk_widget_show_all (GTK_WIDGET(omgr_device));

    OnvifApp__display_device(app,omgr_device);
}

static int OnvifApp__device_already_exist(OnvifApp * app, char * xaddr){
    OnvifAppPrivate *priv = OnvifApp__get_instance_private (app);
    int ret = 0;
    GList * childs = gtk_container_get_children(GTK_CONTAINER(priv->listbox));
    GList * tmp = childs;
    OnvifMgrDeviceRow * device;
    GLIST_FOREACH(device, childs) {
        OnvifDevice * odev = OnvifMgrDeviceRow__get_device(device);
        OnvifDeviceService * devserv = OnvifDevice__get_device_service(odev);
        char * endpoint = OnvifDeviceService__get_endpoint(devserv);
        if(!strcmp(xaddr,endpoint)){
            C_DEBUG("Record already part of list [%s]\n",endpoint);
            ret = 1;
        }
        free(endpoint);
        childs = childs->next;
    }
    g_list_free(tmp);
    return ret;
}

void OnvifApp__create_ui (OnvifApp * app) {
    GtkWidget *main_window;  /* The uppermost window, containing all other windows */
    GtkWidget *grid;
    GtkWidget *widget;
    GtkWidget *label;
    GtkWidget * hbox;
    OnvifAppPrivate *priv = OnvifApp__get_instance_private (app);

    main_window = gtk_window_new (GTK_WINDOW_TOPLEVEL);
    g_signal_connect (G_OBJECT (main_window), "delete-event", G_CALLBACK (OnvifApp__window_delete_event_cb), app);

    gtk_window_set_title (GTK_WINDOW (main_window), "Onvif Device Manager");

    GtkWidget * overlay =gtk_overlay_new();
    gtk_container_add (GTK_CONTAINER (main_window), overlay);

    /* Here we construct the container that is going pack our buttons */
    grid = gtk_grid_new ();
    gtk_container_set_border_width (GTK_CONTAINER (grid), 5);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay),grid);

    AppDialog__add_to_overlay((AppDialog *) priv->profiles_dialog,GTK_OVERLAY(overlay));
    AppDialog__add_to_overlay((AppDialog *) priv->add_dialog,GTK_OVERLAY(overlay));
    AppDialog__add_to_overlay((AppDialog *) priv->cred_dialog,GTK_OVERLAY(overlay));
    AppDialog__add_to_overlay((AppDialog *) priv->msg_dialog, GTK_OVERLAY(overlay));

    GtkWidget * left_grid = gtk_grid_new ();

    hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL,0);
    gtk_grid_attach (GTK_GRID (left_grid), hbox, 0, 0, 1, 1);

    char * btn_css = "* { padding: 0px; font-size: 1.5em; }";
    GtkCssProvider * btn_css_provider = NULL;

    GError *error = NULL;
    GtkWidget * image = GtkStyledImage__new((unsigned char *)_binary_tower_png_start, _binary_tower_png_end - _binary_tower_png_start, 25, 25, error);

    priv->btn_scan = gtk_button_new ();
    // label = gtk_label_new("");
    // gtk_label_set_markup (GTK_LABEL (label), "<span><b><b><b>Scan</b></b></b>...</span>");
    // gtk_label_set_markup (GTK_LABEL (label), "&#x1F50D;"); //Magnifying glass (color)
    // gtk_label_set_markup (GTK_LABEL (label), "&#x1f4e1;"); //Antenna arrow (color)
    // gtk_label_set_markup (GTK_LABEL (label), "&#x2609;"); //Dotted circle (called sun)
    // gtk_label_set_markup (GTK_LABEL (label), "&#x27F2;"); //clockwise Circle arrow (Safe icon theme expectation)
    // gtk_label_set_markup (GTK_LABEL (label), "&#x1f78b;"); //Target (Not center aligned)
    // gtk_container_add (GTK_CONTAINER (app->btn_scan), label);
    if(image){
        gtk_button_set_image(GTK_BUTTON(priv->btn_scan), image);
    } else {
        if(error && error->message){
            C_ERROR("Error creating tower icon : %s",error->message);
        } else {
            C_ERROR("Error creating tower icon : [null]");
        }
        label = gtk_label_new("");
        gtk_label_set_markup (GTK_LABEL (label), "&#x27F2;"); //clockwise Circle arrow (Safe icon theme expectation)
        gtk_container_add (GTK_CONTAINER (priv->btn_scan), label);
    }

    gtk_box_pack_start (GTK_BOX(hbox),priv->btn_scan,TRUE,TRUE,0);
    g_signal_connect (priv->btn_scan, "clicked", G_CALLBACK (OnvifApp__btn_scan_cb), app);
    btn_css_provider = gui_widget_set_css(priv->btn_scan, btn_css, btn_css_provider);


    GtkWidget * add_btn = gtk_button_new ();
    gui_widget_make_square(add_btn);
    label = gtk_label_new("");
    // gtk_label_set_markup (GTK_LABEL (label), "&#x2795;"); //Heavy
    gtk_label_set_markup (GTK_LABEL (label), "&#xFF0B;"); //Full width
    gtk_container_add (GTK_CONTAINER (add_btn), label);
    btn_css_provider = gui_widget_set_css(add_btn, btn_css, btn_css_provider);

    gtk_box_pack_start (GTK_BOX(hbox),add_btn,FALSE,TRUE,0);

    g_signal_connect (add_btn, "clicked", G_CALLBACK (OnvifApp__add_btn_cb), app);

    /* --- Create a list item from the data element --- */
    widget = gtk_scrolled_window_new (NULL, NULL);
    gtk_widget_set_hexpand (widget, TRUE);
    priv->listbox = gtk_list_box_new ();
    //TODO Calculate preferred width to display IP without scrollbar
    gtk_widget_set_size_request(widget,135,-1);
    gtk_widget_set_vexpand (priv->listbox, TRUE);
    gtk_list_box_set_selection_mode (GTK_LIST_BOX (priv->listbox), GTK_SELECTION_SINGLE);
    gtk_container_add(GTK_CONTAINER(widget),priv->listbox);
    gtk_grid_attach (GTK_GRID (left_grid), widget, 0, 2, 1, 1);
    g_signal_connect (priv->listbox, "row-selected", G_CALLBACK (OnvifApp__row_selected_cb), app);
    g_object_unref(gui_widget_set_css(widget, "* { padding-bottom: 3px; padding-top: 3px; padding-right: 3px; }", NULL)); 

    widget = gtk_button_new ();
    label = gtk_label_new("");
    gtk_label_set_markup (GTK_LABEL (label), "<span size=\"medium\">Quit</span>");
    gtk_container_add (GTK_CONTAINER (widget), label);
    gtk_widget_set_vexpand (widget, FALSE);
    gtk_widget_set_hexpand (widget, FALSE);
    g_signal_connect (G_OBJECT(widget), "clicked", G_CALLBACK (OnvifApp__quit_cb), app);
    gtk_grid_attach (GTK_GRID (left_grid), widget, 0, 3, 1, 1);
    g_object_unref(gui_widget_set_css(widget, btn_css, btn_css_provider)); 
    
    GtkWidget *hpaned = gtk_paned_new (GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_vexpand (hpaned, TRUE);
    gtk_widget_set_hexpand (hpaned, TRUE);
    g_object_unref(gui_widget_set_css(hpaned, "paned separator{min-width: 10px;}", NULL));
    gtk_paned_pack1 (GTK_PANED (hpaned), left_grid, FALSE, FALSE);

    /* Create a new notebook, place the position of the tabs */
    priv->main_notebook = gtk_notebook_new ();
    gtk_notebook_set_tab_pos (GTK_NOTEBOOK (priv->main_notebook), GTK_POS_TOP);
    gtk_widget_set_vexpand (priv->main_notebook, TRUE);
    gtk_widget_set_hexpand (priv->main_notebook, TRUE);
    gtk_paned_pack2 (GTK_PANED (hpaned), priv->main_notebook, FALSE, FALSE);

    gtk_grid_attach (GTK_GRID (grid), hpaned, 0, 0, 1, 4);
    gtk_paned_set_wide_handle(GTK_PANED(hpaned),TRUE);
    char * TITLE_STR = "NVT";
    label = gtk_label_new (TITLE_STR);

    //Hidden spinner used to display stream start loading
    priv->player_loading_handle = gtk_spinner_new ();

    //Only show label and keep loading hidden
    hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL,  6);
    gtk_box_pack_start (GTK_BOX(hbox),label,TRUE,TRUE,0);
    gtk_box_pack_start(GTK_BOX(hbox),priv->player_loading_handle,FALSE,FALSE,0);
    gtk_widget_show_all(hbox);

    widget = OnvifNVT__create_ui(priv->player);
    gtk_notebook_append_page (GTK_NOTEBOOK (priv->main_notebook), widget, hbox);

    label = gtk_label_new ("Details");
    //Hidden spinner used to display stream start loading
    widget = gtk_spinner_new ();
    OnvifDetails__set_details_loading_handle(priv->details,widget);

    //Only show label and keep loading hidden
    hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL,  6);
    gtk_box_pack_start (GTK_BOX(hbox),label,TRUE,TRUE,0);
    gtk_box_pack_start(GTK_BOX(hbox),widget,FALSE,FALSE,0);
    gtk_widget_show_all(hbox);

    widget = OnvifDetails__get_widget(priv->details);

    gtk_notebook_append_page (GTK_NOTEBOOK (priv->main_notebook), widget, hbox);


    label = gtk_label_new ("Settings");
    //Hidden spinner used to display stream start loading
    widget = gtk_spinner_new ();
    AppSettings__set_details_loading_handle(priv->settings,widget);

    //Only show label and keep loading hidden
    hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL,  6);
    gtk_box_pack_start (GTK_BOX(hbox),label,TRUE,TRUE,0);
    gtk_box_pack_start(GTK_BOX(hbox),widget,FALSE,FALSE,0);
    gtk_widget_show_all(hbox);

    widget = AppSettings__get_widget(priv->settings);

    gtk_notebook_append_page (GTK_NOTEBOOK (priv->main_notebook), widget, hbox);



    label = gtk_label_new ("Task Manager");
    //Hidden spinner used to display stream start loading
    // widget = gtk_spinner_new ();
    // TaskMgr__set_loading_handle(app->taskmgr,widget);

    //Only show label and keep loading hidden
    hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL,  6);
    gtk_box_pack_start (GTK_BOX(hbox),label,TRUE,TRUE,0);

    int running = EventQueue__get_running_event_count(priv->queue);
    int pending = EventQueue__get_pending_event_count(priv->queue);
    int total = EventQueue__get_thread_count(priv->queue);
    char str[7];
    sprintf(str, "[%d/%d]", running + pending,total);
    priv->task_label = gtk_label_new (str);
    gtk_box_pack_start(GTK_BOX(hbox),priv->task_label,FALSE,FALSE,0);
    // gtk_box_pack_start(GTK_BOX(hbox),widget,FALSE,FALSE,0);
    gtk_widget_show_all(hbox);

    widget = TaskMgr__get_widget(priv->taskmgr);

    gtk_notebook_append_page (GTK_NOTEBOOK (priv->main_notebook), widget, hbox);


    GdkMonitor* monitor = gdk_display_get_primary_monitor(gdk_display_get_default());
    if(monitor){
        GdkRectangle workarea = {0};
        gdk_monitor_get_workarea(monitor,&workarea);
        
        if(workarea.width > 775){//If resolution allows it, big enough to show no scrollbars
            gtk_window_set_default_size(GTK_WINDOW(main_window),775,480);
        } else {//Resolution is so low that we launch fullscreen
            gtk_window_set_default_size(GTK_WINDOW(main_window),workarea.width,workarea.height);
        }
    } else {
        gtk_window_set_default_size(GTK_WINDOW(main_window),540,380);
    }

    priv->window = main_window;
    gtk_widget_show_all (main_window);

    g_signal_connect (G_OBJECT (priv->main_notebook), "switch-page", G_CALLBACK (OnvifApp__switch_page), app);

}

void OnvifApp__dispose(GObject * obj){
    OnvifApp * self = ONVIFMGR_APP(obj);
    OnvifAppPrivate *priv = OnvifApp__get_instance_private (self);
    if (self) {
        //Destroying the queue will hang until all threads are stopped
        CObject__destroy((CObject*)priv->queue);
        OnvifDetails__destroy(priv->details);
        AppSettings__destroy(priv->settings);
        TaskMgr__destroy(priv->taskmgr);
        CObject__destroy((CObject*)priv->profiles_dialog);
        CObject__destroy((CObject*)priv->add_dialog);
        CObject__destroy((CObject*)priv->cred_dialog);
        CObject__destroy((CObject*)priv->msg_dialog);
        //Destroying the player will cause it to hang until its state changed to NULL
        //Destroying the player after the queue because the queue could dispatch retry call
        g_object_unref(priv->player);
        
        //Using idle destruction to allow pending idles to execute
        safely_destroy_widget(priv->window);
    }
}

static void
OnvifApp__class_init (OnvifAppClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    object_class->dispose = OnvifApp__dispose;

    signals[DEVICE_CHANGED] =
        g_signal_newv ("device-changed",
                        G_TYPE_FROM_CLASS (klass),
                        G_SIGNAL_RUN_LAST | G_SIGNAL_NO_RECURSE | G_SIGNAL_NO_HOOKS,
                        NULL /* closure */,
                        NULL /* accumulator */,
                        NULL /* accumulator data */,
                        NULL /* C marshaller */,
                        G_TYPE_NONE /* return_type */,
                        0     /* n_params */,
                        NULL  /* param_types */);
}

static gboolean
OnvifApp_has_owner (COwnableObject *self)
{
    OnvifAppPrivate *priv = OnvifApp__get_instance_private (ONVIFMGR_APP(self));
    return priv->owned;
}

static void
OnvifApp_disown (COwnableObject *self)
{
    OnvifAppPrivate *priv = OnvifApp__get_instance_private (ONVIFMGR_APP(self));
    priv->owned = FALSE;
    g_object_unref(self);
}

static void
OnvifApp__ownable_interface_init (COwnableObjectInterface *iface)
{
    iface->has_owner = OnvifApp_has_owner;
    iface->disown = OnvifApp_disown;
}

static void
OnvifApp__init (OnvifApp *self)
{
    OnvifAppPrivate *priv = OnvifApp__get_instance_private (self);

    priv->device = NULL;
    priv->owned = 1;
    priv->task_label = NULL;
    priv->queue = EventQueue__create(OnvifApp__eq_dispatch_cb,self);
    priv->details = OnvifDetails__create(self);
    priv->settings = AppSettings__create(priv->queue);
    priv->taskmgr = TaskMgr__create();

    AppSettingsStream__set_overscale_callback(priv->settings->stream,OnvifApp__setting_overscale_cb,self);
    priv->current_page = 0;
    priv->player = GstRtspPlayer__new();
    GstRtspPlayer__set_allow_overscale(priv->player,AppSettingsStream__get_allow_overscale(priv->settings->stream));

    //Defaults 8 paralell event threads.
    //TODO support configuration to modify this
    EventQueue__start(priv->queue);
    EventQueue__start(priv->queue);
    EventQueue__start(priv->queue);
    EventQueue__start(priv->queue);
    EventQueue__start(priv->queue);
    EventQueue__start(priv->queue);
    EventQueue__start(priv->queue);
    EventQueue__start(priv->queue);

    g_signal_connect (G_OBJECT(priv->player), "retry", G_CALLBACK (OnvifApp__player_retry_cb), self);
    g_signal_connect (G_OBJECT(priv->player), "error", G_CALLBACK (OnvifApp__player_error_cb), self);
    g_signal_connect (G_OBJECT(priv->player), "stopped", G_CALLBACK (OnvifApp__player_stopped_cb), self);
    g_signal_connect (G_OBJECT(priv->player), "started", G_CALLBACK (OnvifApp__player_started_cb), self);

    priv->profiles_dialog = ProfilesDialog__create(priv->queue, OnvifApp__profile_selected_cb);
    priv->add_dialog = AddDeviceDialog__create();
    priv->cred_dialog = CredentialsDialog__create();
    priv->msg_dialog = MsgDialog__create();
    
    OnvifApp__create_ui (self);

}


OnvifApp * OnvifApp__new(void){
    return g_object_new (ONVIFMGR_TYPE_APP, NULL);
}

void OnvifApp__destroy(OnvifApp* self){
    g_return_if_fail (self != NULL);
    g_return_if_fail (ONVIFMGR_IS_APP (self));
    COwnableObject__disown(COWNABLE_OBJECT(self)); //Changing owned state, flagging pending events to be ignored
    while(G_IS_OBJECT(self) && G_OBJECT(self)->ref_count >= 1){ //Wait for destruction to complete
        usleep(500000);
    }
}

MsgDialog * OnvifApp__get_msg_dialog(OnvifApp * self){
    g_return_val_if_fail (self != NULL, NULL);
    g_return_val_if_fail (ONVIFMGR_IS_APP (self), NULL);
    OnvifAppPrivate *priv = OnvifApp__get_instance_private (self);
    return priv->msg_dialog;
}

OnvifMgrDeviceRow * OnvifApp__get_device(OnvifApp * self){
    g_return_val_if_fail (self != NULL, NULL);
    g_return_val_if_fail (ONVIFMGR_IS_APP (self), NULL);
    OnvifAppPrivate *priv = OnvifApp__get_instance_private (self);
    return priv->device;
}

void OnvifApp__dispatch(OnvifApp* self, void (*callback)(), void * user_data){
    g_return_if_fail (self != NULL);
    g_return_if_fail (ONVIFMGR_IS_APP (self));
    OnvifAppPrivate *priv = OnvifApp__get_instance_private (self);
    EventQueue__insert(priv->queue,callback,user_data);
}