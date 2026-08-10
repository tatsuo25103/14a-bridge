using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.IO;
using System.Runtime.InteropServices;

internal static class CreateMesIcon
{
    [DllImport("user32.dll", CharSet = CharSet.Auto)]
    private static extern bool DestroyIcon(IntPtr handle);

    private static int Main(string[] args)
    {
        if (args.Length != 2) return 1;
        using (var source = new Bitmap(args[0]))
        using (var output = new Bitmap(256, 256))
        using (var graphics = Graphics.FromImage(output))
        {
            graphics.Clear(Color.Black);
            graphics.InterpolationMode = InterpolationMode.HighQualityBicubic;
            float scale = Math.Min(224F / source.Width, 224F / source.Height);
            int width = (int)(source.Width * scale);
            int height = (int)(source.Height * scale);
            graphics.DrawImage(source, (256 - width) / 2, (256 - height) / 2, width, height);
            IntPtr handle = output.GetHicon();
            try
            {
                using (var icon = Icon.FromHandle(handle))
                using (var stream = File.Create(args[1])) icon.Save(stream);
            }
            finally { DestroyIcon(handle); }
        }
        return 0;
    }
}
