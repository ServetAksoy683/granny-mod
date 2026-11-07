package com.dvloper.mod;

import android.util.Base64;

import java.io.BufferedInputStream;
import java.io.ByteArrayOutputStream;
import java.io.EOFException;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.Socket;
import java.security.SecureRandom;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.ThreadFactory;

import javax.net.ssl.HttpsURLConnection;
import javax.net.ssl.SSLSocket;
import javax.net.ssl.SSLSocketFactory;

/** Kucuk WebSocket (wss) istemcisi. Harici kutuphane gerektirmez. */
public final class Net {

    public interface Listener {
        void onOpen();
        void onText(String s);
        void onBinary(byte[] b);
        void onClose(String why);
    }

    private final Listener l;
    private final SecureRandom rnd = new SecureRandom();
    private final Object wlock = new Object();
    // Android ana (UI) is parcaciginda soket yazmak yasak (NetworkOnMainThreadException),
    // bu yuzden tum gonderimler tek bir arka plan is parcaciginda yapilir.
    private final ExecutorService tx = Executors.newSingleThreadExecutor(new ThreadFactory() {
        public Thread newThread(Runnable r) {
            Thread t = new Thread(r, "mod-net-tx");
            t.setDaemon(true);
            return t;
        }
    });
    private volatile Socket sock;
    private volatile OutputStream out;
    private volatile boolean open;

    public Net(Listener l) {
        this.l = l;
    }

    public boolean isOpen() {
        return open;
    }

    public void connect(final String host, final String path) {
        Thread t = new Thread(new Runnable() {
            public void run() {
                runConn(host, path);
            }
        }, "mod-net");
        t.setDaemon(true);
        t.start();
    }

    public boolean sendText(String s) {
        try {
            return submit(1, s.getBytes("UTF-8"));
        } catch (Exception e) {
            return false;
        }
    }

    public boolean sendBinary(byte[] b) {
        return submit(2, b);
    }

    private boolean submit(final int op, final byte[] b) {
        if (!open) return false;
        try {
            tx.execute(new Runnable() {
                public void run() {
                    try {
                        sendFrame(op, b);
                    } catch (Throwable t) {
                    }
                }
            });
            return true;
        } catch (Exception e) {
            return false;
        }
    }

    public void close() {
        open = false;
        try {
            Socket s = sock;
            if (s != null) s.close();
        } catch (Exception e) {
        }
    }

    private void runConn(String host, String path) {
        String why = "kapandi";
        Socket s = null;
        try {
            SSLSocketFactory f = (SSLSocketFactory) SSLSocketFactory.getDefault();
            SSLSocket ss = (SSLSocket) f.createSocket(host, 443);
            s = ss;
            sock = ss;
            ss.setTcpNoDelay(true);
            ss.setSoTimeout(15000); // sadece el sikismasi icin
            ss.startHandshake();
            if (!HttpsURLConnection.getDefaultHostnameVerifier().verify(host, ss.getSession())) {
                throw new IOException("sertifika dogrulanamadi");
            }

            byte[] kb = new byte[16];
            rnd.nextBytes(kb);
            String key = Base64.encodeToString(kb, Base64.NO_WRAP);
            String req = "GET " + path + " HTTP/1.1\r\n"
                    + "Host: " + host + "\r\n"
                    + "Upgrade: websocket\r\n"
                    + "Connection: Upgrade\r\n"
                    + "Sec-WebSocket-Key: " + key + "\r\n"
                    + "Sec-WebSocket-Version: 13\r\n\r\n";
            OutputStream os = ss.getOutputStream();
            os.write(req.getBytes("UTF-8"));
            os.flush();

            InputStream in = new BufferedInputStream(ss.getInputStream());
            String status = readLine(in);
            if (status == null || !status.contains(" 101")) {
                throw new IOException("sunucu cevabi: " + status);
            }
            String line;
            while ((line = readLine(in)) != null && line.length() > 0) {
                // basliklari atla
            }

            ss.setSoTimeout(0);
            out = os;
            open = true;
            startKeepAlive();
            l.onOpen();

            ByteArrayOutputStream frag = null;
            int fragOp = 0;
            while (open) {
                int b0 = in.read();
                if (b0 < 0) break;
                int b1 = readByte(in);
                boolean fin = (b0 & 0x80) != 0;
                int op = b0 & 0x0F;
                long len = b1 & 0x7F;
                if (len == 126) {
                    len = (readByte(in) << 8) | readByte(in);
                } else if (len == 127) {
                    len = 0;
                    for (int i = 0; i < 8; i++) len = (len << 8) | readByte(in);
                }
                if (len > (1 << 20)) throw new IOException("cok buyuk paket");
                boolean masked = (b1 & 0x80) != 0;
                byte[] mask = null;
                if (masked) {
                    mask = new byte[4];
                    readFully(in, mask);
                }
                byte[] p = new byte[(int) len];
                readFully(in, p);
                if (masked) {
                    for (int i = 0; i < p.length; i++) p[i] ^= mask[i & 3];
                }

                if (op == 8) break;
                if (op == 9) {
                    sendFrame(10, p);
                    continue;
                }
                if (op == 10) continue;
                if (op == 1 || op == 2) {
                    if (fin) {
                        deliver(op, p);
                    } else {
                        frag = new ByteArrayOutputStream();
                        fragOp = op;
                        frag.write(p);
                    }
                } else if (op == 0 && frag != null) {
                    frag.write(p);
                    if (fin) {
                        deliver(fragOp, frag.toByteArray());
                        frag = null;
                    }
                }
            }
        } catch (Exception e) {
            String m = e.getMessage();
            why = (m != null && m.length() > 0) ? m : e.getClass().getSimpleName();
        } finally {
            open = false;
            try {
                tx.shutdown();
            } catch (Exception e) {
            }
            try {
                if (s != null) s.close();
            } catch (Exception e) {
            }
            l.onClose(why);
        }
    }

    private void deliver(int op, byte[] p) throws IOException {
        if (op == 1) l.onText(new String(p, "UTF-8"));
        else l.onBinary(p);
    }

    private void startKeepAlive() {
        Thread ka = new Thread(new Runnable() {
            public void run() {
                while (open) {
                    try {
                        Thread.sleep(20000);
                    } catch (InterruptedException e) {
                        return;
                    }
                    if (!open) return;
                    sendFrame(9, new byte[0]);
                }
            }
        }, "mod-net-ka");
        ka.setDaemon(true);
        ka.start();
    }

    private boolean sendFrame(int op, byte[] payload) {
        synchronized (wlock) {
            OutputStream os = out;
            if (!open || os == null) return false;
            try {
                int n = payload.length;
                ByteArrayOutputStream f = new ByteArrayOutputStream(n + 14);
                f.write(0x80 | op);
                if (n < 126) {
                    f.write(0x80 | n);
                } else if (n < 65536) {
                    f.write(0x80 | 126);
                    f.write((n >> 8) & 0xFF);
                    f.write(n & 0xFF);
                } else {
                    f.write(0x80 | 127);
                    for (int i = 7; i >= 0; i--) f.write((int) (((long) n >> (8 * i)) & 0xFF));
                }
                byte[] mask = new byte[4];
                rnd.nextBytes(mask);
                f.write(mask);
                byte[] m = new byte[n];
                for (int i = 0; i < n; i++) m[i] = (byte) (payload[i] ^ mask[i & 3]);
                f.write(m);
                os.write(f.toByteArray());
                os.flush();
                return true;
            } catch (IOException e) {
                close();
                return false;
            }
        }
    }

    private static int readByte(InputStream in) throws IOException {
        int b = in.read();
        if (b < 0) throw new EOFException();
        return b;
    }

    private static void readFully(InputStream in, byte[] b) throws IOException {
        int off = 0;
        while (off < b.length) {
            int r = in.read(b, off, b.length - off);
            if (r < 0) throw new EOFException();
            off += r;
        }
    }

    private static String readLine(InputStream in) throws IOException {
        StringBuilder sb = new StringBuilder();
        int c;
        while ((c = in.read()) >= 0) {
            if (c == '\n') break;
            if (c != '\r') sb.append((char) c);
        }
        if (c < 0 && sb.length() == 0) return null;
        return sb.toString();
    }
}
