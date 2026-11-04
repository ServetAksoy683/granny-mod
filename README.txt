Granny MP mod (v0.1: menu + oda baglantisi)
1) Bu klasoru GitHub'da yeni bir depoya yukle (.github klasoru dahil).
2) Actions sekmesi -> build-dex -> Run workflow (ya da push sonrasi kendiliginden calisir).
3) Biten calismadan "classes6-dex" artifact'ini indir, icindeki classes6.dex dosyasini APK'nin kokune ekle.
4) SafeDKApplication <clinit> icine Mod.init() cagrisini ekle (SafeDKApplication_patched.smali.txt).
