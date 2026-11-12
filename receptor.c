#include <gst/gst.h>

typedef struct {
    GMainLoop  *loop;
    GstElement *jitterbuffer;
    gint        porta;
} App;

static GstElement *criar(const char *fabrica, const char *nome)
{
    GstElement *e = gst_element_factory_make(fabrica, nome);
    if (e == NULL)
        g_printerr("ERRO: elemento '%s' nao encontrado. Confira a instalacao "
                   "dos plugins do GStreamer.\n", fabrica);
    return e;
}

static GstPadProbeReturn sonda_perdas(GstPad *pad, GstPadProbeInfo *info, gpointer dados)
{
    GstEvent *ev = GST_PAD_PROBE_INFO_EVENT(info);
    (void)pad;
    (void)dados;

    if (GST_EVENT_TYPE(ev) == GST_EVENT_CUSTOM_DOWNSTREAM &&
        gst_event_has_name(ev, "GstRTPPacketLost")) {
        const GstStructure *s = gst_event_get_structure(ev);
        guint seq = 0;
        guint64 duracao = 0;
        gst_structure_get_uint(s, "seqnum", &seq);
        gst_structure_get_uint64(s, "duration", &duracao);
        g_print("PERDA: pacote %u nao chegou a tempo (%.1f ms de audio)\n",
                seq, duracao / 1e6);
    }
    return GST_PAD_PROBE_OK;
}

static gboolean mostrar_estatisticas(gpointer dados)
{
    App *app = dados;
    GstStructure *st = NULL;
    guint64 entregues = 0, perdidos = 0, atrasados = 0, jitter = 0;

    g_object_get(app->jitterbuffer, "stats", &st, NULL);
    gst_structure_get_uint64(st, "num-pushed", &entregues);
    gst_structure_get_uint64(st, "num-lost", &perdidos);
    gst_structure_get_uint64(st, "num-late", &atrasados);
    gst_structure_get_uint64(st, "avg-jitter", &jitter);
    gst_structure_free(st);

    guint64 total = entregues + perdidos;
    g_print("entregues=%" G_GUINT64_FORMAT "  perdidos=%" G_GUINT64_FORMAT " (%.2f%%)"
            "  atrasados=%" G_GUINT64_FORMAT "  jitter medio=%.1f ms\n",
            entregues, perdidos, total ? 100.0 * perdidos / total : 0.0,
            atrasados, jitter / 1e6);
    return G_SOURCE_CONTINUE;
}

static gboolean ao_receber_mensagem(GstBus *bus, GstMessage *msg, gpointer dados)
{
    App *app = dados;
    (void)bus;

    switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_ERROR: {
        GError *erro;
        gchar *detalhes;
        gst_message_parse_error(msg, &erro, &detalhes);
        g_printerr("ERRO em %s: %s\n", GST_OBJECT_NAME(msg->src), erro->message);
        if (g_strcmp0(GST_OBJECT_NAME(msg->src), "udp") == 0)
            g_printerr("Dica: a porta %d pode estar em uso por outro programa.\n", app->porta);
        g_error_free(erro);
        g_free(detalhes);
        g_main_loop_quit(app->loop);
        break;
    }
    case GST_MESSAGE_ELEMENT:
        if (gst_message_has_name(msg, "GstUDPSrcTimeout"))
            g_print("AVISO: nenhum pacote recebido. O transmissor esta ligado?\n");
        break;
    case GST_MESSAGE_EOS:
        g_main_loop_quit(app->loop);
        break;
    default:
        break;
    }
    return TRUE;
}

int main(int argc, char *argv[])
{
    App app = { 0 };

    gst_init(&argc, &argv);

    GOptionContext *ctx = g_option_context_new("- recebe audio por RTP/UDP");
    g_option_context_add_main_entries(ctx, opcoes, NULL);
    GError *erro = NULL;
    if (!g_option_context_parse(ctx, &argc, &argv, &erro)) {
        g_printerr("ERRO nas opcoes: %s\n", erro->message);
        return 1;
    }
    if (app.porta < 1 || app.porta > 65535 || latencia < 0) {
        g_printerr("ERRO: opcoes invalidas. Use --help.\n");
        return 1;
    }

    GstElement *pipeline = gst_pipeline_new("receptor");
    GstElement *udp      = criar("udpsrc", "udp");
    GstElement *jb       = criar("rtpjitterbuffer", "jitterbuffer");
    GstElement *depay    = criar("rtpL16depay", "rtp");
    GstElement *conv     = criar("audioconvert", "conversor");
    GstElement *ream     = criar("audioresample", "reamostragem");
    GstElement *saida    = criar("autoaudiosink", "saida");
    if (!pipeline || !udp || !jb || !depay || !conv || !ream || !saida)
        return 1;
    app.jitterbuffer = jb;

    GstCaps *caps = gst_caps_from_string(
        "application/x-rtp, media=audio, clock-rate=48000, encoding-name=L16, "
        "encoding-params=1, channels=1, payload=96");
    g_object_set(udp, "port", app.porta, "caps", caps,
                 "reuse", FALSE,
                 "timeout", (guint64)(3 * GST_SECOND),
                 NULL);


    GstPad *pad = gst_element_get_static_pad(jb, "src");
    gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM, sonda_perdas, NULL, NULL);
    gst_object_unref(pad);

    app.loop = g_main_loop_new(NULL, FALSE);
    GstBus *bus = gst_element_get_bus(pipeline);
    gst_bus_add_watch(bus, ao_receber_mensagem, &app);
    gst_object_unref(bus);

    if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        g_printerr("ERRO: a pipeline nao iniciou (porta em uso ou sem saida de audio?).\n");
        while (g_main_context_iteration(NULL, FALSE))
            ;
        return 1;
    }
    g_print("Recebendo na porta %d, latencia do jitterbuffer = %d ms. Ctrl+C para encerrar.\n",
            app.porta, latencia);
    g_timeout_add(1000, mostrar_estatisticas, &app);

    g_main_loop_run(app.loop);

    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    g_main_loop_unref(app.loop);
    return 0;
}
