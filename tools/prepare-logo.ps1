$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$source = Join-Path $PSScriptRoot '..\assets\dvd-video-logo.png'
$output = Join-Path $PSScriptRoot '..\assets\dvd-video-logo.bmp'
$source = [IO.Path]::GetFullPath($source)
$output = [IO.Path]::GetFullPath($output)

$code = @'
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;

public static class DvdLogoTextureBuilder
{
    public static void Build(string source, string output)
    {
        using (Bitmap input = new Bitmap(source))
        using (Bitmap mask = new Bitmap(512, 512, PixelFormat.Format32bppArgb))
        using (Graphics graphics = Graphics.FromImage(mask))
        {
            graphics.Clear(Color.Transparent);
            graphics.InterpolationMode =
                System.Drawing.Drawing2D.InterpolationMode.HighQualityBicubic;
            graphics.DrawImage(input, new Rectangle(0, 0, 512, 307));

            Rectangle bounds = new Rectangle(0, 0, mask.Width, mask.Height);
            BitmapData data = mask.LockBits(bounds, ImageLockMode.ReadWrite,
                                            PixelFormat.Format32bppArgb);
            try
            {
                int stride = data.Stride;
                byte[] pixels = new byte[stride * mask.Height];
                Marshal.Copy(data.Scan0, pixels, 0, pixels.Length);
                for (int y = 0; y < mask.Height; ++y)
                {
                    int row = y * stride;
                    for (int x = 0; x < mask.Width; ++x)
                    {
                        int pixel = row + x * 4;
                        if (pixels[pixel + 3] != 0)
                        {
                            pixels[pixel] = 255;
                            pixels[pixel + 1] = 255;
                            pixels[pixel + 2] = 255;
                        }
                    }
                }
                Marshal.Copy(pixels, 0, data.Scan0, pixels.Length);
            }
            finally
            {
                mask.UnlockBits(data);
            }
            mask.Save(output, ImageFormat.Bmp);
        }
    }
}
'@

Add-Type -TypeDefinition $code -ReferencedAssemblies 'System.Drawing'
[DvdLogoTextureBuilder]::Build($source, $output)
Write-Host "Generated $output"
