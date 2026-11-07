package com.dvloper.mod;

import android.app.Activity;
import android.app.AlertDialog;
import android.app.Application;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.DialogInterface;
import android.content.SharedPreferences;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.text.InputFilter;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import org.json.JSONArray;
import org.json.JSONObject;

import java.net.URLEncoder;
import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.TreeMap;

/** Granny multiplayer modu: menu ve oda baglantisi. */
public final class Mod {

    private static final String HOST = "granny-mp-relay.shubmodentertainment.workers.dev";
    private static final Handler UI = new Handler(Looper.getMainLooper());

    private static boolean started;
    private static volatile Activity current;
    private static Button fab;
    private static AlertDialog dialog;
    private static TextView logView;
    private static volatile Net net;
    private static volatile int myId;
    private static int hostId;
    private static String roomCode = "";
    private static final Map<Integer, String> players = new TreeMap<Integer, String>();
    private static final List<String> logLines = new ArrayList<String>();
    private static final Map<Integer, float[]> remote = new TreeMap<Integer, float[]>();
    private static long lastUi;
    private static boolean nativeOk;
    private static final Map<Integer, TextView> tags = new HashMap<Integer, TextView>();

    private static native void nativeSetRemote(int id, float x, float y, float z);

    private static native void nativeRemoveRemote(int id);

    private static native void nativeClearRemote();

    private static native void nativeStartGame();

    private Mod() {
    }

    /** SafeDKApplication.<clinit> icinden cagrilir. */
    public static synchronized void init() {
        if (started) return;
        started = true;
        try {
            System.loadLibrary("modlib");
            nativeOk = true;
        } catch (Throwable t) {
            log("Native yüklenemedi: " + t);
        }
        Thread t = new Thread(new Runnable() {
            public void run() {
                waitForApp();
            }
        }, "mod-init");
        t.setDaemon(true);
        t.start();
    }

    private static final Application.ActivityLifecycleCallbacks CB = new Application.ActivityLifecycleCallbacks() {
        public void onActivityCreated(Activity a, Bundle b) {
        }

        public void onActivityStarted(Activity a) {
        }

        public void onActivityResumed(Activity a) {
            if (a.getClass().getName().endsWith("UnityPlayerActivity")) attach(a);
        }

        public void onActivityPaused(Activity a) {
        }

        public void onActivityStopped(Activity a) {
        }

        public void onActivitySaveInstanceState(Activity a, Bundle b) {
        }

        public void onActivityDestroyed(Activity a) {
            if (a == current) {
                try {
                    if (dialog != null && dialog.isShowing()) dialog.dismiss();
                } catch (Throwable t) {
                }
                current = null;
                fab = null;
            }
        }
    };

    private static void waitForApp() {
        Application a = null;
        for (int i = 0; i < 400 && a == null; i++) {
            try {
                Class<?> c = Class.forName("android.app.ActivityThread");
                a = (Application) c.getMethod("currentApplication").invoke(null);
            } catch (Throwable t) {
            }
            if (a == null) {
                try {
                    Thread.sleep(50);
                } catch (InterruptedException e) {
                    return;
                }
            }
        }
        if (a == null) return;
        final Application app = a;
        UI.post(new Runnable() {
            public void run() {
                app.registerActivityLifecycleCallbacks(CB);
            }
        });

        // Yedek: aktiviteyi dogrudan bulmayi dene
        for (int i = 0; i < 40 && current == null; i++) {
            try {
                Thread.sleep(500);
            } catch (InterruptedException e) {
                return;
            }
            try {
                Class<?> c = Class.forName("com.unity3d.player.UnityPlayer");
                final Object act = c.getField("currentActivity").get(null);
                if (act instanceof Activity && current == null) {
                    UI.post(new Runnable() {
                        public void run() {
                            attach((Activity) act);
                        }
                    });
                    break;
                }
            } catch (Throwable t) {
            }
        }
    }

    private static void attach(final Activity a) {
        if (a == current && fab != null && fab.getParent() != null) return;
        current = a;
        tags.clear();
        try {
            ViewGroup decor = (ViewGroup) a.getWindow().getDecorView();
            if (fab != null && fab.getParent() instanceof ViewGroup) {
                ((ViewGroup) fab.getParent()).removeView(fab);
            }
            Button b = new Button(a);
            b.setText("MP");
            b.setAllCaps(false);
            b.setAlpha(0.75f);
            b.setOnClickListener(new View.OnClickListener() {
                public void onClick(View v) {
                    showMenu();
                }
            });
            FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(dp(a, 64), dp(a, 44),
                    Gravity.TOP | Gravity.START);
            lp.leftMargin = dp(a, 8);
            lp.topMargin = dp(a, 8);
            decor.addView(b, lp);
            fab = b;
        } catch (Throwable t) {
        }
    }

    private static void showMenu() {
        final Activity a = current;
        if (a == null) return;
        if (dialog != null && dialog.isShowing()) return;
        final SharedPreferences sp = a.getSharedPreferences("granny_mp", 0);

        LinearLayout root = new LinearLayout(a);
        root.setOrientation(LinearLayout.VERTICAL);
        int pad = dp(a, 16);
        root.setPadding(pad, pad, pad, pad);

        TextView title = new TextView(a);
        title.setText("Granny Multiplayer");
        title.setTextSize(18);
        root.addView(title);

        final EditText nameEt = new EditText(a);
        nameEt.setHint("Adın");
        nameEt.setSingleLine(true);
        nameEt.setFilters(new InputFilter[]{new InputFilter.LengthFilter(16)});
        nameEt.setText(sp.getString("name", ""));
        root.addView(nameEt);

        Button create = new Button(a);
        create.setText("Oda kur");
        create.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                String nm = cleanName(nameEt.getText().toString());
                sp.edit().putString("name", nm).apply();
                connect("/create?name=" + enc(nm));
            }
        });
        root.addView(create);

        final EditText codeEt = new EditText(a);
        codeEt.setHint("Oda kodu");
        codeEt.setSingleLine(true);
        codeEt.setFilters(new InputFilter[]{new InputFilter.LengthFilter(6)});
        root.addView(codeEt);

        Button join = new Button(a);
        join.setText("Odaya katıl");
        join.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                String code = codeEt.getText().toString().trim().toUpperCase(Locale.ENGLISH);
                if (code.length() < 4) {
                    log("Geçerli bir oda kodu gir");
                    return;
                }
                String nm = cleanName(nameEt.getText().toString());
                sp.edit().putString("name", nm).apply();
                connect("/join/" + enc(code) + "?name=" + enc(nm));
            }
        });
        root.addView(join);

        Button start = new Button(a);
        start.setText("Oyunu başlat (host)");
        start.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                Net n = net;
                if (n == null || !n.isOpen() || myId <= 0) {
                    log("Önce bir odaya bağlan");
                    return;
                }
                if (myId != hostId) {
                    log("Sadece host oyunu başlatabilir");
                    return;
                }
                try {
                    JSONObject o = new JSONObject();
                    o.put("t", "start");
                    n.sendText(o.toString());
                } catch (Exception e) {
                }
                log("Oyun başlatılıyor (zorluk: menüdeki seçimin)");
                nStart();
            }
        });
        root.addView(start);

        Button test = new Button(a);
        test.setText("Test mesajı gönder");
        test.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                Net n = net;
                if (n != null && n.isOpen() && myId > 0) {
                    try {
                        JSONObject o = new JSONObject();
                        o.put("t", "chat");
                        o.put("m", "merhaba");
                        n.sendText(o.toString());
                        log("Sen: merhaba");
                    } catch (Exception e) {
                        log("Gönderilemedi");
                    }
                } else {
                    log("Önce bir odaya bağlan");
                }
            }
        });
        root.addView(test);

        Button leave = new Button(a);
        leave.setText("Odadan ayrıl");
        leave.setOnClickListener(new View.OnClickListener() {
            public void onClick(View v) {
                if (net != null) net.close();
            }
        });
        root.addView(leave);

        TextView lv = new TextView(a);
        lv.setTextSize(12);
        lv.setPadding(0, dp(a, 8), 0, 0);
        root.addView(lv);
        logView = lv;

        ScrollView sv = new ScrollView(a);
        sv.addView(root);

        dialog = new AlertDialog.Builder(a).setView(sv).setNegativeButton("Kapat", null).create();
        dialog.setOnDismissListener(new DialogInterface.OnDismissListener() {
            public void onDismiss(DialogInterface d) {
                logView = null;
            }
        });
        dialog.show();
        refreshLog();
    }

    private static void connect(String path) {
        final Net old = net;
        final Net[] holder = new Net[1];
        Net n = new Net(new Net.Listener() {
            public void onOpen() {
                if (net == holder[0]) log("Bağlandı, oda bilgisi bekleniyor...");
            }

            public void onText(String s) {
                if (net == holder[0]) handleText(s);
            }

            public void onBinary(byte[] b) {
                if (net == holder[0]) handleBinary(b);
            }

            public void onClose(String why) {
                if (net != holder[0]) return;
                synchronized (players) {
                    players.clear();
                }
                synchronized (remote) {
                    remote.clear();
                }
                nClear();
                clearTags();
                myId = 0;
                roomCode = "";
                log("Bağlantı kapandı: " + why);
            }
        });
        holder[0] = n;
        net = n;
        if (old != null) old.close();
        log("Bağlanıyor...");
        n.connect(HOST, path);
    }

    /** Native taraftan cagrilir (oyun ana is parcacigi, ~10 Hz). */
    public static void onLocalPos(float x, float y, float z, float yaw) {
        Net n = net;
        if (n == null || !n.isOpen() || myId <= 0) return;
        ByteBuffer bb = ByteBuffer.allocate(17);
        bb.put((byte) 1).putFloat(x).putFloat(y).putFloat(z).putFloat(yaw);
        n.sendBinary(bb.array());
    }

    /** Native taraftan mesaj yazdirir. */
    public static void nativeLog(String s) {
        log(s);
    }

    private static void handleBinary(byte[] b) {
        if (b.length >= 18 && b[1] == 1) {
            ByteBuffer bb = ByteBuffer.wrap(b, 2, 16);
            float[] p = new float[]{bb.getFloat(), bb.getFloat(), bb.getFloat(), bb.getFloat()};
            synchronized (remote) {
                remote.put(b[0] & 0xFF, p);
            }
            nSet(b[0] & 0xFF, p[0], p[1], p[2]);
            long now = System.currentTimeMillis();
            if (now - lastUi > 250 && logView != null) {
                lastUi = now;
                UI.post(new Runnable() {
                    public void run() {
                        refreshLog();
                    }
                });
            }
        }
    }

    private static void nSet(int id, float x, float y, float z) {
        if (!nativeOk) return;
        try {
            nativeSetRemote(id, x, y, z);
        } catch (Throwable t) {
        }
    }

    private static void nRemove(int id) {
        if (!nativeOk) return;
        try {
            nativeRemoveRemote(id);
        } catch (Throwable t) {
        }
    }

    private static void nStart() {
        if (!nativeOk) {
            log("Native yüklü değil");
            return;
        }
        try {
            nativeStartGame();
        } catch (Throwable t) {
            log("Başlatılamadı: " + t);
        }
    }

    private static void promptStart() {
        UI.post(new Runnable() {
            public void run() {
                Activity a = current;
                if (a == null) {
                    log("Katılma penceresi açılamadı (aktivite yok)");
                    return;
                }
                try {
                    new AlertDialog.Builder(a)
                            .setTitle("Oyun başlıyor")
                            .setMessage("Host oyunu başlattı. Katılmak ister misin?")
                            .setPositiveButton("Katıl", new DialogInterface.OnClickListener() {
                                public void onClick(DialogInterface d, int w) {
                                    nStart();
                                }
                            })
                            .setNegativeButton("İptal", null)
                            .show();
                } catch (Throwable t) {
                    log("Katılma penceresi hatası: " + t);
                }
            }
        });
    }

    private static void nClear() {
        if (!nativeOk) return;
        try {
            nativeClearRemote();
        } catch (Throwable t) {
        }
    }

    /** Native taraftan cagrilir: uzak oyuncunun ekran konumu (nick etiketi icin). */
    public static void onRemoteScreen(final int id, final float sx, final float sy, final float sz,
                                      final float sw, final float sh, final float dist) {
        UI.post(new Runnable() {
            public void run() {
                updateTag(id, sx, sy, sz, sw, sh, dist);
            }
        });
    }

    private static void updateTag(int id, float sx, float sy, float sz, float sw, float sh, float dist) {
        Activity a = current;
        if (a == null) return;
        TextView tv = tags.get(id);
        if (tv == null) {
            try {
                tv = new TextView(a);
                tv.setTextColor(0xFFFFFFFF);
                tv.setTextSize(14);
                tv.setShadowLayer(4, 0, 0, 0xFF000000);
                tv.setBackgroundColor(0x66000000);
                tv.setPadding(dp(a, 6), dp(a, 2), dp(a, 6), dp(a, 2));
                ViewGroup decor = (ViewGroup) a.getWindow().getDecorView();
                decor.addView(tv, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
                        ViewGroup.LayoutParams.WRAP_CONTENT));
                tags.put(id, tv);
            } catch (Throwable t) {
                return;
            }
        }
        String nm;
        synchronized (players) {
            nm = players.get(id);
        }
        if (nm == null) nm = "#" + id;
        tv.setText(nm + " (" + (int) dist + "m)");
        if (sz <= 0 || sw <= 0 || sh <= 0) {
            tv.setVisibility(View.GONE);
            return;
        }
        View decor = a.getWindow().getDecorView();
        float x = sx / sw * decor.getWidth();
        float y = (1f - sy / sh) * decor.getHeight();
        tv.setVisibility(View.VISIBLE);
        tv.setX(x - tv.getWidth() / 2f);
        tv.setY(y - tv.getHeight());
    }

    private static void removeTag(final int id) {
        UI.post(new Runnable() {
            public void run() {
                TextView tv = tags.remove(id);
                if (tv != null && tv.getParent() instanceof ViewGroup) {
                    ((ViewGroup) tv.getParent()).removeView(tv);
                }
            }
        });
    }

    private static void clearTags() {
        UI.post(new Runnable() {
            public void run() {
                for (TextView tv : new ArrayList<TextView>(tags.values())) {
                    if (tv.getParent() instanceof ViewGroup) ((ViewGroup) tv.getParent()).removeView(tv);
                }
                tags.clear();
            }
        });
    }

    private static void handleText(String s) {
        try {
            JSONObject o = new JSONObject(s);
            String t = o.optString("t");
            if ("welcome".equals(t)) {
                myId = o.getInt("id");
                roomCode = o.getString("code");
                hostId = o.getInt("host");
                synchronized (players) {
                    players.clear();
                    JSONArray arr = o.getJSONArray("players");
                    for (int i = 0; i < arr.length(); i++) {
                        JSONObject p = arr.getJSONObject(i);
                        players.put(p.getInt("id"), p.optString("name", "?"));
                    }
                }
                log("Oda kodu: " + roomCode + "  | sen #" + myId + (myId == hostId ? " (HOST)" : ""));
                copyToClipboard(roomCode);
            } else if ("join".equals(t)) {
                int id = o.getInt("id");
                String nm = o.optString("name", "?");
                synchronized (players) {
                    players.put(id, nm);
                }
                log("#" + id + " " + nm + " katıldı");
            } else if ("leave".equals(t)) {
                int id = o.getInt("id");
                synchronized (players) {
                    players.remove(id);
                }
                synchronized (remote) {
                    remote.remove(id);
                }
                nRemove(id);
                removeTag(id);
                hostId = o.optInt("host", hostId);
                log("#" + id + " ayrıldı" + (hostId == myId ? " (artık HOST sensin)" : ""));
            } else if ("start".equals(t)) {
                int from = o.optInt("from");
                log("Başlat mesajı geldi (gönderen #" + from + ", host #" + hostId + ")");
                if (from == hostId) {
                    log("Host oyunu başlattı, oyuna katılınıyor...");
                    nStart();
                }
            } else if ("chat".equals(t)) {
                log("#" + o.optInt("from") + ": " + o.optString("m"));
            }
        } catch (Exception e) {
            log("Geçersiz mesaj");
        }
    }

    private static void copyToClipboard(final String text) {
        UI.post(new Runnable() {
            public void run() {
                try {
                    Activity a = current;
                    if (a == null) return;
                    ClipboardManager cm = (ClipboardManager) a.getSystemService(Context.CLIPBOARD_SERVICE);
                    cm.setPrimaryClip(ClipData.newPlainText("oda", text));
                } catch (Throwable t) {
                }
            }
        });
    }

    private static void log(String s) {
        synchronized (logLines) {
            logLines.add(s);
            while (logLines.size() > 16) logLines.remove(0);
        }
        UI.post(new Runnable() {
            public void run() {
                refreshLog();
            }
        });
    }

    private static void refreshLog() {
        TextView tv = logView;
        if (tv == null) return;
        StringBuilder sb = new StringBuilder();
        synchronized (logLines) {
            for (String x : logLines) sb.append(x).append('\n');
        }
        synchronized (players) {
            if (!players.isEmpty()) {
                sb.append("Oyuncular: ");
                boolean first = true;
                for (Map.Entry<Integer, String> e : players.entrySet()) {
                    if (!first) sb.append(", ");
                    first = false;
                    sb.append('#').append(e.getKey()).append(' ').append(e.getValue());
                    if (e.getKey() == hostId) sb.append(" (HOST)");
                }
            }
        }
        synchronized (remote) {
            for (Map.Entry<Integer, float[]> e : remote.entrySet()) {
                float[] p = e.getValue();
                sb.append("\n#").append(e.getKey()).append(String.format(Locale.US,
                        " konum: %.1f %.1f %.1f  yön: %.0f", p[0], p[1], p[2], p[3]));
            }
        }
        tv.setText(sb.toString());
    }

    private static String cleanName(String s) {
        String t = s == null ? "" : s.trim();
        if (t.length() == 0) t = "Oyuncu";
        if (t.length() > 16) t = t.substring(0, 16);
        return t;
    }

    private static String enc(String s) {
        try {
            return URLEncoder.encode(s, "UTF-8");
        } catch (Exception e) {
            return "";
        }
    }

    private static int dp(Context c, int v) {
        return (int) (v * c.getResources().getDisplayMetrics().density + 0.5f);
    }
}
