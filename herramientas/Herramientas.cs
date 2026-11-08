// Helpers for instalar.ps1, compiled at run time by PowerShell (Add-Type).
// Written for C# 5 (the compiler that ships with Windows PowerShell 5.1).
//
//  - Cd.CopyFolder:   copies a folder out of an ISO 9660 image (.iso, or the
//                     data track of a .bin/.cue), without mounting it.
//  - Cd.ExtractAudio: writes the audio tracks of a .bin/.cue image as WAV.
//  - Ayuda.Convert:   converts a WinHelp 4 .HLP file into one HTML page.
using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;

namespace EvoFix
{
    // ------------------------------------------------------------------ CD image
    public static class Cd
    {
        class Track { public int Num; public string Type; public long Index0 = -1; public long Index1 = -1; }

        class Image
        {
            public string BinPath;
            public int SectorSize;   // 2352 (raw) or 2048 (cooked)
            public int DataOffset;   // user data offset inside a sector
            public List<Track> Tracks = new List<Track>();
        }

        static long Msf(string s)
        {
            string[] p = s.Split(':');
            return (long.Parse(p[0]) * 60 + long.Parse(p[1])) * 75 + long.Parse(p[2]);
        }

        static Image Open(string path)
        {
            Image img = new Image();
            string ext = Path.GetExtension(path).ToLowerInvariant();
            if (ext == ".cue")
            {
                foreach (string raw in File.ReadAllLines(path, Encoding.GetEncoding(1252)))
                {
                    string line = raw.Trim();
                    Match m = Regex.Match(line, "^FILE\\s+\"(.+)\"\\s+BINARY", RegexOptions.IgnoreCase);
                    if (m.Success)
                    {
                        string bin = Path.Combine(Path.GetDirectoryName(path), m.Groups[1].Value);
                        if (img.BinPath != null) throw new Exception("Solo se admiten imágenes .cue con un único archivo .bin.");
                        img.BinPath = bin;
                        continue;
                    }
                    m = Regex.Match(line, "^TRACK\\s+(\\d+)\\s+(\\S+)", RegexOptions.IgnoreCase);
                    if (m.Success)
                    {
                        Track t = new Track();
                        t.Num = int.Parse(m.Groups[1].Value);
                        t.Type = m.Groups[2].Value.ToUpperInvariant();
                        img.Tracks.Add(t);
                        continue;
                    }
                    m = Regex.Match(line, "^INDEX\\s+(\\d+)\\s+(\\S+)", RegexOptions.IgnoreCase);
                    if (m.Success && img.Tracks.Count > 0)
                    {
                        int n = int.Parse(m.Groups[1].Value);
                        if (n == 0) img.Tracks[img.Tracks.Count - 1].Index0 = Msf(m.Groups[2].Value);
                        if (n == 1) img.Tracks[img.Tracks.Count - 1].Index1 = Msf(m.Groups[2].Value);
                    }
                }
                if (img.BinPath == null || !File.Exists(img.BinPath))
                    throw new Exception("No se encuentra el .bin indicado en el .cue.");
                string type = img.Tracks.Count > 0 ? img.Tracks[0].Type : "MODE1/2352";
                img.SectorSize = type.EndsWith("/2048") ? 2048 : 2352;
                img.DataOffset = type.StartsWith("MODE2") && img.SectorSize == 2352 ? 24 : (img.SectorSize == 2352 ? 16 : 0);
            }
            else
            {
                img.BinPath = path;
                img.SectorSize = 2048;
                img.DataOffset = 0;
                using (FileStream f = File.OpenRead(path))
                {
                    if (f.Length % 2352 == 0)
                    {
                        byte[] b = new byte[12];
                        f.Read(b, 0, 12);
                        if (b[0] == 0 && b[1] == 0xFF && b[11] == 0) { img.SectorSize = 2352; img.DataOffset = 16; }
                    }
                }
            }
            return img;
        }

        class Entry { public string Name; public long Extent; public long Size; public bool Dir; }

        static byte[] ReadSectors(FileStream f, Image img, long first, long bytes)
        {
            byte[] outb = new byte[bytes];
            long done = 0, sector = first;
            byte[] buf = new byte[img.SectorSize];
            while (done < bytes)
            {
                f.Position = sector * img.SectorSize;
                f.Read(buf, 0, img.SectorSize);
                int n = (int)Math.Min(2048, bytes - done);
                Buffer.BlockCopy(buf, img.DataOffset, outb, (int)done, n);
                done += n;
                sector++;
            }
            return outb;
        }

        static List<Entry> ReadDir(FileStream f, Image img, long extent, long size)
        {
            List<Entry> list = new List<Entry>();
            byte[] d = ReadSectors(f, img, extent, size);
            int o = 0;
            while (o < d.Length)
            {
                int len = d[o];
                if (len == 0) { o = (o / 2048 + 1) * 2048; continue; }
                int nl = d[o + 32];
                byte[] nb = new byte[nl];
                Buffer.BlockCopy(d, o + 33, nb, 0, nl);
                if (!(nl == 1 && (nb[0] == 0 || nb[0] == 1)))
                {
                    Entry e = new Entry();
                    e.Name = Encoding.GetEncoding(1252).GetString(nb).Split(';')[0];
                    if (e.Name.EndsWith(".")) e.Name = e.Name.Substring(0, e.Name.Length - 1);
                    e.Extent = BitConverter.ToUInt32(d, o + 2);
                    e.Size = BitConverter.ToUInt32(d, o + 10);
                    e.Dir = (d[o + 25] & 2) != 0;
                    list.Add(e);
                }
                o += len;
            }
            return list;
        }

        // Copies <folder> (e.g. "EVOLUTION") from the image root into dest.
        // Returns the number of files copied.
        public static int CopyFolder(string imagePath, string folder, string dest)
        {
            Image img = Open(imagePath);
            using (FileStream f = File.OpenRead(img.BinPath))
            {
                byte[] pvd = ReadSectors(f, img, 16, 2048);
                if (pvd[1] != 'C' || pvd[2] != 'D' || pvd[3] != '0' || pvd[4] != '0' || pvd[5] != '1')
                    throw new Exception("La imagen no contiene un sistema de archivos ISO 9660 legible.");
                long rootExt = BitConverter.ToUInt32(pvd, 156 + 2);
                long rootSize = BitConverter.ToUInt32(pvd, 156 + 10);
                foreach (Entry e in ReadDir(f, img, rootExt, rootSize))
                    if (e.Dir && string.Equals(e.Name, folder, StringComparison.OrdinalIgnoreCase))
                        return CopyTree(f, img, e, dest);
            }
            throw new Exception("No se encuentra la carpeta " + folder + " en la imagen.");
        }

        static int CopyTree(FileStream f, Image img, Entry dir, string dest)
        {
            int count = 0;
            Directory.CreateDirectory(dest);
            foreach (Entry e in ReadDir(f, img, dir.Extent, dir.Size))
            {
                string target = Path.Combine(dest, e.Name);
                if (e.Dir) { count += CopyTree(f, img, e, target); continue; }
                using (FileStream o = File.Create(target))
                {
                    long left = e.Size, sector = e.Extent;
                    while (left > 0)
                    {
                        long chunk = Math.Min(left, 2048L * 512);
                        byte[] b = ReadSectors(f, img, sector, chunk);
                        o.Write(b, 0, b.Length);
                        left -= chunk;
                        sector += (chunk + 2047) / 2048;
                    }
                }
                count++;
            }
            return count;
        }

        static double Rms(FileStream f, long fromFrame, long frames)
        {
            if (fromFrame < 0 || frames <= 0) return 0;
            byte[] b = new byte[frames * 2352];
            f.Position = fromFrame * 2352;
            int n = f.Read(b, 0, b.Length);
            double sum = 0;
            int count = 0;
            for (int i = 0; i + 1 < n; i += 4) { short s = BitConverter.ToInt16(b, i); sum += (double)s * s; count++; }
            return count > 0 ? Math.Sqrt(sum / count) : 0;
        }

        // Writes every audio track as "<prefix>NN.wav" (NN = CD track number) and
        // returns how many were written. Some rips mark a track's INDEX 01 two
        // seconds late, so the previous track ends with the start of the next
        // one: when the 2 s before INDEX 01 have sound but the second before
        // them is silent, the track is started 2 s earlier.
        public static int ExtractAudio(string cuePath, string destDir, string prefix)
        {
            Image img = Open(cuePath);
            if (img.SectorSize != 2352) throw new Exception("La imagen no tiene pistas de audio en bruto.");
            Directory.CreateDirectory(destDir);
            int written = 0;
            using (FileStream f = File.OpenRead(img.BinPath))
            {
                long total = f.Length / 2352;
                List<long> starts = new List<long>();
                for (int i = 0; i < img.Tracks.Count; i++)
                {
                    Track t = img.Tracks[i];
                    long s = t.Index1;
                    if (t.Type == "AUDIO" && i > 0 && img.Tracks[i - 1].Type == "AUDIO" && t.Index0 < 0)
                    {
                        if (Rms(f, s - 150, 150) > 200 && Rms(f, s - 225, 75) < 30) s -= 150;
                    }
                    starts.Add(s);
                }
                for (int i = 0; i < img.Tracks.Count; i++)
                {
                    Track t = img.Tracks[i];
                    if (t.Type != "AUDIO") continue;
                    long start = starts[i];
                    long end = i + 1 < img.Tracks.Count
                        ? (img.Tracks[i + 1].Index0 >= 0 ? img.Tracks[i + 1].Index0 : starts[i + 1])
                        : total;
                    long bytes = (end - start) * 2352;
                    string name = Path.Combine(destDir, prefix + t.Num.ToString("00") + ".wav");
                    using (FileStream o = File.Create(name))
                    using (BinaryWriter w = new BinaryWriter(o))
                    {
                        w.Write(Encoding.ASCII.GetBytes("RIFF")); w.Write((uint)(36 + bytes));
                        w.Write(Encoding.ASCII.GetBytes("WAVEfmt ")); w.Write(16); w.Write((short)1); w.Write((short)2);
                        w.Write(44100); w.Write(44100 * 4); w.Write((short)4); w.Write((short)16);
                        w.Write(Encoding.ASCII.GetBytes("data")); w.Write((uint)bytes);
                        f.Position = start * 2352;
                        byte[] buf = new byte[2352 * 75];
                        long left = bytes;
                        while (left > 0)
                        {
                            int n = f.Read(buf, 0, (int)Math.Min(buf.Length, left));
                            if (n <= 0) break;
                            w.Write(buf, 0, n);
                            left -= n;
                        }
                    }
                    written++;
                }
            }
            return written;
        }
    }

    // ------------------------------------------------------------------ WinHelp
    public static class Ayuda
    {
        static readonly Encoding Ansi = Encoding.GetEncoding(1252);

        class Rd
        {
            public byte[] B; public int P;
            public Rd(byte[] b, int p) { B = b; P = p; }
            public int U8() { return B[P++]; }
            public int U16() { int v = BitConverter.ToUInt16(B, P); P += 2; return v; }
            public int I16() { int v = BitConverter.ToInt16(B, P); P += 2; return v; }
            public uint U32() { uint v = BitConverter.ToUInt32(B, P); P += 4; return v; }
            public int CShort() { return (B[P] & 1) != 0 ? (U16() >> 1) - 0x4000 : (U8() >> 1) - 0x40; }
            public int CUShort() { return (B[P] & 1) != 0 ? U16() >> 1 : U8() >> 1; }
            public long CLong() { return (B[P] & 1) != 0 ? (long)(U32() >> 1) - 0x40000000 : (U16() >> 1) - 0x4000; }
            public long CULong() { return (B[P] & 1) != 0 ? (long)(U32() >> 1) : U16() >> 1; }
        }

        class Hlp
        {
            public byte[] D;
            public Dictionary<string, int> Files = new Dictionary<string, int>();
            public Hlp(string path)
            {
                D = System.IO.File.ReadAllBytes(path);
                if (BitConverter.ToUInt32(D, 0) != 0x35F3F) throw new Exception("No es un archivo de ayuda de WinHelp.");
                foreach (int q in Leaves(BitConverter.ToInt32(D, 4)))
                {
                    int e = Array.IndexOf(D, (byte)0, q);
                    Files[Ansi.GetString(D, q, e - q)] = BitConverter.ToInt32(D, e + 1);
                }
            }
            // Positions of the leaf entries of a B+ tree; each caller knows its entry layout.
            public IEnumerable<int> Leaves(int at)
            {
                int o = at + 9;
                int pagesize = BitConverter.ToUInt16(D, o + 4);
                o += 6 + 16;
                int root = BitConverter.ToInt16(D, o + 4);
                int levels = BitConverter.ToInt16(D, o + 10);
                int baseOff = o + 16;
                int page = root;
                for (int l = 0; l < levels - 1; l++) page = BitConverter.ToInt16(D, baseOff + page * pagesize + 4);
                List<int> result = new List<int>();
                while (page != -1)
                {
                    int p = baseOff + page * pagesize;
                    int n = BitConverter.ToInt16(D, p + 2);
                    int next = BitConverter.ToInt16(D, p + 6);
                    int q = p + 8;
                    for (int i = 0; i < n; i++)
                    {
                        result.Add(q);
                        q = SkipEntry(at, q);
                    }
                    page = next;
                }
                return result;
            }
            Dictionary<int, int> entrySize = new Dictionary<int, int>();
            public int Kind; // 0 directory, 1 ttl, 2 context (set before Leaves)
            int SkipEntry(int tree, int q)
            {
                if (Kind == 0) return Array.IndexOf(D, (byte)0, q) + 5;
                if (Kind == 1) return Array.IndexOf(D, (byte)0, q + 4) + 1;
                return q + 8;
            }
            public byte[] File(string name)
            {
                int o = Files[name];
                int used = BitConverter.ToInt32(D, o + 4);
                byte[] b = new byte[used];
                Buffer.BlockCopy(D, o + 9, b, 0, used);
                return b;
            }
        }

        static byte[] Lz77(byte[] src)
        {
            List<byte> o = new List<byte>();
            int i = 0;
            while (i < src.Length)
            {
                int bits = src[i++];
                for (int k = 0; k < 8 && i < src.Length; k++)
                {
                    if ((bits & (1 << k)) != 0)
                    {
                        int w = BitConverter.ToUInt16(src, i); i += 2;
                        int dist = (w & 0xFFF) + 1, n = (w >> 12) + 3;
                        for (int j = 0; j < n; j++) o.Add(dist <= o.Count ? o[o.Count - dist] : (byte)0);
                    }
                    else o.Add(src[i++]);
                }
            }
            return o.ToArray();
        }

        static byte[] Rle(byte[] src)
        {
            List<byte> o = new List<byte>();
            int i = 0;
            while (i < src.Length)
            {
                int c = src[i++];
                if ((c & 0x80) != 0)
                {
                    int n = c & 0x7F;
                    for (int j = 0; j < n && i < src.Length; j++) o.Add(src[i++]);
                }
                else
                {
                    if (i < src.Length) for (int j = 0; j < c; j++) o.Add(src[i]);
                    i++;
                }
            }
            return o.ToArray();
        }

        static byte[] Unpack(byte[] raw, int packing)
        {
            if (packing == 1) return Rle(raw);
            if (packing == 2) return Lz77(raw);
            if (packing == 3) return Rle(Lz77(raw));
            return raw;
        }

        static byte[] Slice(byte[] b, long off, long len)
        {
            if (off < 0) off = 0;
            if (off > b.Length) off = b.Length;
            if (off + len > b.Length) len = b.Length - off;
            byte[] r = new byte[Math.Max(0, len)];
            Buffer.BlockCopy(b, (int)off, r, 0, r.Length);
            return r;
        }

        public static uint TopicHash(string name)
        {
            uint h = 0;
            foreach (byte ch in Ansi.GetBytes(name.ToUpperInvariant()))
            {
                int v = ch == '_' ? 13 : ch == '0' ? 10 : ch - 0x30;
                h = unchecked(h * 43 + (uint)v);
            }
            return h;
        }

        // ---------------------------------------------------------- pictures
        static byte[] Bmp(int w, int h, int bits, byte[] palette, int colors, byte[] raw, int ppm)
        {
            MemoryStream ms = new MemoryStream();
            BinaryWriter bw = new BinaryWriter(ms);
            int offbits = 14 + 40 + palette.Length;
            bw.Write((byte)'B'); bw.Write((byte)'M'); bw.Write(offbits + raw.Length); bw.Write(0); bw.Write(offbits);
            bw.Write(40); bw.Write(w); bw.Write(h); bw.Write((short)1); bw.Write((short)bits); bw.Write(0);
            bw.Write(raw.Length); bw.Write(ppm); bw.Write(ppm); bw.Write(colors); bw.Write(0);
            bw.Write(palette); bw.Write(raw);
            return ms.ToArray();
        }

        static byte[] PictureToBmp(byte[] data)
        {
            if (data.Length < 8) return null;
            int magic = BitConverter.ToUInt16(data, 0), n = BitConverter.ToUInt16(data, 2);
            if ((magic != 0x506C && magic != 0x706C) || n < 1) return null;
            int off = (int)BitConverter.ToUInt32(data, 4);
            Rd r = new Rd(data, off);
            int ptype = r.U8(), packing = r.U8();
            if (ptype == 8) return MetafileToBmp(data, off, r, packing);
            if (ptype != 5 && ptype != 6) return null;
            r.CULong(); r.CULong(); r.CUShort();
            int bits = r.CUShort();
            int width = (int)r.CULong(), height = (int)r.CULong();
            int used = (int)r.CULong(); r.CULong();
            long csize = r.CULong(); r.CULong();
            long coff = r.U32(); r.U32();
            int ncolors = used != 0 ? used : (bits <= 8 ? 1 << bits : 0);
            byte[] palette = (ptype == 6 || bits <= 8) ? Slice(data, r.P, 4 * ncolors) : new byte[0];
            if (ptype == 5 && bits == 1 && palette.Length == 0) { palette = new byte[] { 0, 0, 0, 0, 255, 255, 255, 0 }; ncolors = 2; }
            byte[] raw = Unpack(Slice(data, off + coff, csize), packing);
            int stride = ((width * bits + 31) / 32) * 4;
            byte[] img = new byte[stride * height];
            if (ptype == 5)
            {
                int ddb = ((width * bits + 15) / 16) * 2;
                for (int y = 0; y < height; y++)
                {
                    int src = y * ddb, dst = (height - 1 - y) * stride;
                    int len = Math.Min(ddb, Math.Max(0, raw.Length - src));
                    if (len > 0) Buffer.BlockCopy(raw, src, img, dst, Math.Min(len, stride));
                }
            }
            else Buffer.BlockCopy(raw, 0, img, 0, Math.Min(raw.Length, img.Length));
            return Bmp(width, height, bits, palette, ncolors, img, 2835);
        }

        [StructLayout(LayoutKind.Sequential)]
        struct METAFILEPICT { public int mm; public int xExt; public int yExt; public IntPtr hMF; }
        [StructLayout(LayoutKind.Sequential)]
        struct BITMAPINFOHEADER
        {
            public int biSize; public int biWidth; public int biHeight; public short biPlanes; public short biBitCount;
            public int biCompression; public int biSizeImage; public int biXPelsPerMeter; public int biYPelsPerMeter;
            public int biClrUsed; public int biClrImportant;
        }
        [StructLayout(LayoutKind.Sequential)]
        struct RECT { public int left, top, right, bottom; }
        [DllImport("gdi32.dll")] static extern IntPtr SetWinMetaFileBits(uint size, byte[] bits, IntPtr hdc, ref METAFILEPICT mfp);
        [DllImport("gdi32.dll")] static extern IntPtr CreateCompatibleDC(IntPtr hdc);
        [DllImport("gdi32.dll")] static extern IntPtr CreateDIBSection(IntPtr hdc, ref BITMAPINFOHEADER bmi, uint usage, out IntPtr bits, IntPtr section, uint offset);
        [DllImport("gdi32.dll")] static extern IntPtr SelectObject(IntPtr hdc, IntPtr obj);
        [DllImport("gdi32.dll")] static extern bool PlayEnhMetaFile(IntPtr hdc, IntPtr emf, ref RECT rc);
        [DllImport("gdi32.dll")] static extern bool DeleteEnhMetaFile(IntPtr emf);
        [DllImport("gdi32.dll")] static extern bool DeleteObject(IntPtr obj);
        [DllImport("gdi32.dll")] static extern bool DeleteDC(IntPtr hdc);
        [DllImport("gdi32.dll")] static extern IntPtr GetStockObject(int obj);
        [DllImport("user32.dll")] static extern int FillRect(IntPtr hdc, ref RECT rc, IntPtr brush);

        static byte[] MetafileToBmp(byte[] data, int off, Rd r, int packing)
        {
            int mapmode = r.CUShort();
            int w = r.U16(), h = r.U16();
            r.CULong();
            long csize = r.CULong(); r.CULong();
            long coff = r.U32();
            byte[] wmf = Unpack(Slice(data, off + coff, csize), packing);
            int px, py;
            if (mapmode == 7 || mapmode == 8)
            {
                px = Math.Max(1, (int)Math.Round(w * 96.0 / 2540, MidpointRounding.ToEven));
                py = Math.Max(1, (int)Math.Round(h * 96.0 / 2540, MidpointRounding.ToEven));
            }
            else { px = Math.Max(1, w); py = Math.Max(1, h); }
            METAFILEPICT mfp = new METAFILEPICT();
            mfp.mm = mapmode;
            mfp.xExt = (mapmode == 7 || mapmode == 8) ? w : 0;
            mfp.yExt = (mapmode == 7 || mapmode == 8) ? h : 0;
            IntPtr emf = SetWinMetaFileBits((uint)wmf.Length, wmf, IntPtr.Zero, ref mfp);
            if (emf == IntPtr.Zero) return null;
            BITMAPINFOHEADER bih = new BITMAPINFOHEADER();
            bih.biSize = 40; bih.biWidth = px; bih.biHeight = py; bih.biPlanes = 1; bih.biBitCount = 24;
            IntPtr bits;
            IntPtr dc = CreateCompatibleDC(IntPtr.Zero);
            IntPtr dib = CreateDIBSection(dc, ref bih, 0, out bits, IntPtr.Zero, 0);
            IntPtr old = SelectObject(dc, dib);
            RECT rc = new RECT(); rc.right = px; rc.bottom = py;
            FillRect(dc, ref rc, GetStockObject(0));
            PlayEnhMetaFile(dc, emf, ref rc);
            int stride = ((px * 3 + 3) / 4) * 4;
            byte[] raw = new byte[stride * py];
            Marshal.Copy(bits, raw, 0, raw.Length);
            SelectObject(dc, old); DeleteObject(dib); DeleteDC(dc); DeleteEnhMetaFile(emf);
            return Bmp(px, py, 24, new byte[0], 0, raw, 3780);
        }

        // ------------------------------------------------------------ topics
        class Rec { public int Type; public byte[] L1; public byte[] L2; public long Offset; }
        class Topic { public string Title; public long Offset; public string Id; public List<Rec> Records = new List<Rec>(); }

        static List<Topic> ReadTopics(Hlp hf)
        {
            byte[] topic = hf.File("|TOPIC");
            byte[] sys = hf.File("|SYSTEM");
            int minor = BitConverter.ToUInt16(sys, 2);
            int flags = BitConverter.ToUInt16(sys, 10);
            int block = (minor > 16 && (flags == 0 || flags == 4)) ? 4096 : 2048;
            bool compressed = minor > 16 && (flags == 4 || flags == 8);
            int dec = compressed ? 0x4000 : block - 12;
            List<byte> space = new List<byte>();
            for (int k = 0; k < topic.Length; k += block)
            {
                byte[] data = Slice(topic, k + 12, block - 12);
                if (compressed) data = Lz77(data);
                byte[] pad = new byte[dec];
                Buffer.BlockCopy(data, 0, pad, 0, Math.Min(data.Length, dec));
                space.AddRange(pad);
            }
            byte[] sp = space.ToArray();
            List<Topic> topics = new List<Topic>();
            HashSet<long> seen = new HashSet<long>();
            long pos = BitConverter.ToInt32(topic, 4);
            long curBlock = -1, count = 0;
            while (pos != -1 && !seen.Contains(pos))
            {
                seen.Add(pos);
                long o = (pos >> 14) * dec + (pos & 0x3FFF) - 12;
                if (o < 0 || o + 21 > sp.Length) break;
                int bsize = BitConverter.ToInt32(sp, (int)o);
                int next = BitConverter.ToInt32(sp, (int)o + 12);
                int dlen1 = BitConverter.ToInt32(sp, (int)o + 16);
                int rtype = sp[o + 20];
                byte[] ld1 = Slice(sp, o + 21, dlen1 - 21);
                byte[] ld2 = Slice(sp, o + dlen1, bsize - dlen1);
                long blk = pos >> 14;
                if (blk != curBlock) { curBlock = blk; count = 0; }
                long offset = (blk << 15) + count;
                if (rtype == 2)
                {
                    int e = Array.IndexOf(ld2, (byte)0);
                    Topic t = new Topic();
                    t.Title = Ansi.GetString(ld2, 0, e < 0 ? ld2.Length : e);
                    t.Offset = offset;
                    topics.Add(t);
                }
                else if (rtype == 0x20 || rtype == 0x23)
                {
                    Rd r = new Rd(ld1, 0);
                    r.CLong();
                    count += r.CUShort();
                    if (topics.Count > 0)
                    {
                        Rec rec = new Rec(); rec.Type = rtype; rec.L1 = ld1; rec.L2 = ld2; rec.Offset = offset;
                        topics[topics.Count - 1].Records.Add(rec);
                    }
                }
                pos = next;
            }
            return topics;
        }

        static string Esc(string s)
        {
            return s.Replace("&", "&amp;").Replace("<", "&lt;").Replace(">", "&gt;").Replace("\"", "&quot;").Replace("'", "&#x27;");
        }

        static string RenderRecord(Rec rec, Func<uint, string> link)
        {
            Rd r = new Rd(rec.L1, 0);
            r.CLong();
            r.CUShort();
            if (rec.Type == 0x23)
            {
                int ncols = r.U8(), ttype = r.U8();
                if (ttype == 0 || ttype == 2) r.I16();
                for (int i = 0; i < ncols; i++) { r.I16(); r.I16(); }
            }
            List<string> texts = new List<string>();
            int st = 0;
            for (int i = 0; i <= rec.L2.Length; i++)
                if (i == rec.L2.Length || rec.L2[i] == 0) { texts.Add(Ansi.GetString(rec.L2, st, i - st)); st = i + 1; }
            int ti = 0;
            Func<string> text = delegate { string s = ti < texts.Count ? texts[ti] : ""; ti++; return Esc(s); };
            List<string> output = new List<string>();
            bool openLink = false;
            try
            {
                bool first = true;
                while (r.P < rec.L1.Length && (rec.Type == 0x23 || first))
                {
                    first = false;
                    if (rec.Type == 0x23 && r.I16() == -1) break;
                    r.P += rec.Type == 0x23 ? 4 : 1;
                    r.U8(); r.U16();
                    int bits = r.U16();
                    if ((bits & 0x0001) != 0) r.CLong();
                    foreach (int fl in new int[] { 0x02, 0x04, 0x08, 0x10, 0x20, 0x40 }) if ((bits & fl) != 0) r.CShort();
                    if ((bits & 0x0100) != 0) { r.U8(); r.I16(); }
                    if ((bits & 0x0200) != 0)
                    {
                        int ntabs = r.CShort();
                        for (int i = 0; i < ntabs; i++) { int t = r.CUShort(); if ((t & 0x4000) != 0) r.CUShort(); }
                    }
                    StringBuilder para = new StringBuilder(text());
                    while (r.P < rec.L1.Length)
                    {
                        int c = r.U8();
                        if (c == 0xFF) break;
                        if (c == 0x80) r.U16();
                        else if (c == 0x81) para.Append("<br>");
                        else if (c == 0x82) { output.Add("<p>" + para + "</p>"); para.Length = 0; }
                        else if (c == 0x83) para.Append("&emsp;");
                        else if (c == 0x86 || c == 0x87 || c == 0x88)
                        {
                            int ptype = r.U8();
                            long size = r.CLong();
                            if (ptype == 0x22) r.CUShort();
                            if (ptype == 0x03 || ptype == 0x22)
                            {
                                Rd pr = new Rd(rec.L1, r.P);
                                pr.I16();
                                int num = pr.U16();
                                string cls = c == 0x86 ? "inl" : c == 0x87 ? "left" : "right";
                                para.Append("<img class=\"" + cls + "\" src=\"img/bm" + num + ".bmp\" alt=\"\">");
                            }
                            r.P += (int)size;
                        }
                        else if (c == 0x89) { if (openLink) { para.Append("</a>"); openLink = false; } }
                        else if (c == 0x8B) para.Append("&nbsp;");
                        else if (c == 0x8C) para.Append("-");
                        else if (c == 0xC8 || c == 0xCC) { int n = r.I16(); r.P += n; }
                        else if (c == 0xE0 || c == 0xE1 || c == 0xE2 || c == 0xE3 || c == 0xE6 || c == 0xE7)
                        {
                            uint hsh = r.U32();
                            para.Append("<a href=\"#" + link(hsh) + "\">");
                            openLink = true;
                        }
                        else if (c == 0xEA || c == 0xEB || c == 0xEE || c == 0xEF)
                        {
                            int n = r.I16(); r.P += n;
                            para.Append("<a>");
                            openLink = true;
                        }
                        else break;
                        para.Append(text());
                    }
                    if (para.ToString().Trim().Length > 0) output.Add("<p>" + para + "</p>");
                }
                if (openLink) output.Add("</a>");
            }
            catch (ArgumentException) { }
            catch (IndexOutOfRangeException) { }
            return string.Join("\n", output.ToArray());
        }

        // Converts hlpPath into outDir\index.html (+ outDir\img\bmN.bmp).
        // Returns a short summary.
        public static string Convert(string hlpPath, string outDir, string cntPath)
        {
            Hlp hf = new Hlp(hlpPath);
            List<Topic> topics = ReadTopics(hf);
            hf.Kind = 2;
            Dictionary<uint, int> contexts = new Dictionary<uint, int>();
            foreach (int q in hf.Leaves(hf.Files["|CONTEXT"]))
                contexts[BitConverter.ToUInt32(hf.D, q)] = BitConverter.ToInt32(hf.D, q + 4);

            List<KeyValuePair<long, string>> starts = new List<KeyValuePair<long, string>>();
            for (int i = 0; i < topics.Count; i++)
            {
                topics[i].Id = "t" + i;
                starts.Add(new KeyValuePair<long, string>(topics[i].Offset, topics[i].Id));
                foreach (Rec rec in topics[i].Records)
                    if (rec.Offset != topics[i].Offset) starts.Add(new KeyValuePair<long, string>(rec.Offset, "o" + rec.Offset));
            }
            starts.Sort(delegate(KeyValuePair<long, string> a, KeyValuePair<long, string> b) { return a.Key.CompareTo(b.Key); });
            Func<long, string> anchorFor = delegate(long off)
            {
                string best = null;
                foreach (KeyValuePair<long, string> kv in starts) { if (kv.Key <= off) best = kv.Value; else break; }
                return best;
            };
            Func<uint, string> link = delegate(uint h)
            {
                int off;
                string a = contexts.TryGetValue(h, out off) ? anchorFor(off) : null;
                return a ?? "top";
            };

            Directory.CreateDirectory(Path.Combine(outDir, "img"));
            int pictures = 0;
            foreach (string name in hf.Files.Keys)
            {
                Match m = Regex.Match(name, "^\\|bm(\\d+)$");
                if (!m.Success) continue;
                byte[] bmp = PictureToBmp(hf.File(name));
                if (bmp == null) continue;
                System.IO.File.WriteAllBytes(Path.Combine(outDir, "img", "bm" + m.Groups[1].Value + ".bmp"), bmp);
                pictures++;
            }

            List<string> toc = new List<string>();
            if (cntPath != null && System.IO.File.Exists(cntPath))
            {
                foreach (string line in System.IO.File.ReadAllLines(cntPath, Ansi))
                {
                    Match m = Regex.Match(line, "^(\\d+)\\s+(.*?)(?:=(.*))?\\s*$");
                    if (!m.Success) continue;
                    int level = int.Parse(m.Groups[1].Value);
                    string label = m.Groups[2].Value;
                    if (m.Groups[3].Success)
                    {
                        string target = m.Groups[3].Value.Split('@')[0].Split('>')[0];
                        int off;
                        string a = contexts.TryGetValue(TopicHash(target), out off) ? anchorFor(off) : null;
                        toc.Add("<li class=\"l" + level + "\"><a href=\"#" + (a ?? "top") + "\">" + Esc(label) + "</a></li>");
                    }
                    else toc.Add("<li class=\"h" + level + "\">" + Esc(label) + "</li>");
                }
            }
            if (toc.Count == 0)
                foreach (Topic t in topics) toc.Add("<li><a href=\"#" + t.Id + "\">" + Esc(t.Title) + "</a></li>");

            List<string> sections = new List<string>();
            foreach (Topic t in topics)
            {
                List<string> parts = new List<string>();
                foreach (Rec rec in t.Records)
                {
                    if (rec.Offset != t.Offset) parts.Add("<span id=\"o" + rec.Offset + "\"></span>");
                    parts.Add(RenderRecord(rec, link));
                }
                sections.Add("<section id=\"" + t.Id + "\"><h2>" + Esc(t.Title) + "</h2>\n" + string.Join("\n", parts.ToArray()) +
                             "\n<p class=\"up\"><a href=\"#top\">Índice</a></p></section>");
            }
            string page = "<!doctype html>\n<html lang=\"es\"><head><meta charset=\"utf-8\"><title>Ayuda de Evolution</title>\n<style>\n" +
                "body{font:15px/1.5 Segoe UI,Arial,sans-serif;max-width:860px;margin:0 auto;padding:16px;background:#fbfaf6;color:#222}\n" +
                "h1{font-size:24px} h2{font-size:19px;border-bottom:1px solid #ccc;padding-bottom:4px;margin-top:40px}\n" +
                "nav ul{list-style:none;padding:0} nav li.h1{font-weight:bold;margin-top:10px} nav li.l2{margin-left:18px}\n" +
                "img.left{float:left;margin:0 12px 8px 0} img.right{float:right;margin:0 0 8px 12px} img{max-width:100%}\n" +
                "section{clear:both} p.up{font-size:13px;text-align:right} a{color:#1a5fb4}\n" +
                "</style></head><body>\n<h1 id=\"top\">Ayuda de Evolution</h1>\n<nav><ul>\n" + string.Join("\n", toc.ToArray()) +
                "\n</ul></nav>\n" + string.Join("\n", sections.ToArray()) + "\n</body></html>\n";
            System.IO.File.WriteAllText(Path.Combine(outDir, "index.html"), page, new UTF8Encoding(false));
            return topics.Count + " temas, " + toc.Count + " entradas de índice, " + pictures + " imágenes";
        }
    }
}
