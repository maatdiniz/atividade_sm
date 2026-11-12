#include <gst/gst.h>
#include <string.h>

static GstElement *criar(const char *fabrica, const char *nome)
{
    GstElement *e = gst_element_factory_make(fabrica, nome);
    if (e == NULL)
        g_printerr("ERRO: elemento '%s' nao encontrado. Confira a instalacao "
                   "dos plugins do GStreamer.\n", fabrica);
    return e;
}

static gboolean ao_receber_mensagem(GstBus *bus, GstMessage *msg, gpointer dados)
{
    GMainLoop *loop = dados;
    (void)bus;

    switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_ERROR: {
        GError *erro;
        gchar *detalhes;
        gst_message_parse_error(msg, &erro, &detalhes);
        g_printerr("ERRO em %s: %s\n", GST_OBJECT_NAME(msg->src), erro->message);
        g_error_free(erro);
        g_free(detalhes);
        g_main_loop_quit(loop);
        break;
    }
    case GST_MESSAGE_EOS:
        g_print("Fim do fluxo.\n");
        g_main_loop_quit(loop);
        break;
    default:
        break;
    }
    return TRUE;
}

int main(int argc, char *argv[])
{
    gchar   *host   = "127.0.0.1";
    gint     porta  = 5000;
    gchar   *fonte  = "teste";
    gdouble  perda  = 0;
    gint     atraso = 0;

    GOptionEntry opcoes[] = {
        { "host",   0, 0, G_OPTION_ARG_STRING, &host,   "IP de destino (padrao 127.0.0.1)", "IP" },
        { "porta",  0, 0, G_OPTION_ARG_INT,    &porta,  "Porta UDP de destino (padrao 5000)", "N" },
        { "fonte",  0, 0, G_OPTION_ARG_STRING, &fonte,  "teste (tom de 440 Hz) ou microfone", "TIPO" },
        { "perda",  0, 0, G_OPTION_ARG_DOUBLE, &perda,  "Rede simulada: porcentagem de pacotes descartados", "PCT" },
        { "atraso", 0, 0, G_OPTION_ARG_INT,    &atraso, "Rede simulada: atraso aleatorio de 0 a N ms", "MS" },
        { NULL }
    };

    gst_init(&argc, &argv);

    GOptionContext *ctx = g_option_context_new("- envia audio por RTP/UDP");
    g_option_context_add_main_entries(ctx, opcoes, NULL);
    GError *erro = NULL;
    if (!g_option_context_parse(ctx, &argc, &argv, &erro)) {
        g_printerr("ERRO nas opcoes: %s\n", erro->message);
        return 1;
    }
    if (porta < 1 || porta > 65535 || perda < 0 || perda > 100 || atraso < 0 ||
        (strcmp(fonte, "teste") != 0 && strcmp(fonte, "microfone") != 0)) {
        g_printerr("ERRO: opcoes invalidas. Use --help.\n");
        return 1;
    }
    gboolean rede_simulada = perda > 0 || atraso > 0;

    GstElement *pipeline = gst_pipeline_new("transmissor");
    GstElement *src      = strcmp(fonte, "microfone") == 0
                           ? criar("autoaudiosrc", "fonte")
                           : criar("audiotestsrc", "fonte");
    GstElement *conv     = criar("audioconvert", "conversor");
    GstElement *ream     = criar("audioresample", "reamostragem");
    GstElement *filtro   = criar("capsfilter", "formato");
    GstElement *pay      = criar("rtpL16pay", "rtp");
    GstElement *netsim   = rede_simulada ? criar("netsim", "rede-simulada") : NULL;
    GstElement *udp      = criar("udpsink", "udp");

    if (!pipeline || !src || !conv || !ream || !filtro || !pay || !udp ||
        (rede_simulada && !netsim))
        return 1;

    if (strcmp(fonte, "teste") == 0)
        g_object_set(src, "is-live", TRUE, "freq", 440.0,
                     "samplesperbuffer", 480, NULL);

    GstCaps *caps = gst_caps_from_string(
        "audio/x-raw, format=S16BE, layout=interleaved, rate=48000, channels=1");
    g_object_set(filtro, "caps", caps, NULL);
    gst_caps_unref(caps);

    g_object_set(pay, "pt", 96,
                 "min-ptime", (gint64)(10 * GST_MSECOND),
                 "max-ptime", (gint64)(10 * GST_MSECOND), NULL);

    if (netsim)
        g_object_set(netsim,
                     "drop-probability",  (gfloat)(perda / 100.0),
                     "delay-probability", (gfloat)(atraso > 0 ? 1.0 : 0.0),
                     "min-delay", 0, "max-delay", atraso, NULL);

    g_object_set(udp, "host", host, "port", porta, "sync", FALSE, NULL);

    gst_bin_add_many(GST_BIN(pipeline), src, conv, ream, filtro, pay, udp, NULL);
    gboolean ok = gst_element_link_many(src, conv, ream, filtro, pay, NULL);
    if (netsim) {
        gst_bin_add(GST_BIN(pipeline), netsim);
        ok = ok && gst_element_link_many(pay, netsim, udp, NULL);
    } else {
        ok = ok && gst_element_link(pay, udp);
    }
    if (!ok) {
        g_printerr("ERRO: nao foi possivel ligar os elementos.\n");
        return 1;
    }

    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    GstBus *bus = gst_element_get_bus(pipeline);
    gst_bus_add_watch(bus, ao_receber_mensagem, loop);
    gst_object_unref(bus);

    if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        g_printerr("ERRO: a pipeline nao iniciou (microfone disponivel?).\n");
        return 1;
    }
    g_print("Transmitindo (%s) para %s:%d", fonte, host, porta);
    if (rede_simulada)
        g_print("  [rede simulada: perda %.1f%%, atraso 0-%d ms]", perda, atraso);
    g_print("\nCtrl+C para encerrar.\n");

    g_main_loop_run(loop);

    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    g_main_loop_unref(loop);
    return 0;
}
