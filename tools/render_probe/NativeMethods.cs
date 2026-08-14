// Windows-only interop and pixel analysis used by run.ps1.
// This file has no entry point; PowerShell compiles and calls it at runtime.
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
using System.Text;

namespace RenderProbe
{
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT
    {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;

        public int Width
        {
            get { return Right - Left; }
        }

        public int Height
        {
            get { return Bottom - Top; }
        }
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct POINT
    {
        public int X;
        public int Y;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct MONITORINFO
    {
        public int cbSize;
        public RECT rcMonitor;
        public RECT rcWork;
        public uint dwFlags;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    internal struct MONITORINFOEX
    {
        public int cbSize;
        public RECT rcMonitor;
        public RECT rcWork;
        public uint dwFlags;

        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)]
        public string szDevice;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    internal struct DISPLAY_DEVICE
    {
        public int cb;

        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)]
        public string DeviceName;

        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)]
        public string DeviceString;

        public int StateFlags;

        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)]
        public string DeviceID;

        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)]
        public string DeviceKey;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    internal struct DEVMODE
    {
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)]
        public string dmDeviceName;

        public short dmSpecVersion;
        public short dmDriverVersion;
        public short dmSize;
        public short dmDriverExtra;
        public int dmFields;

        // Anonymous union: dmPosition is the active member for display devices.
        public int dmPositionX;
        public int dmPositionY;
        public int dmDisplayOrientation;
        public int dmDisplayFixedOutput;

        public short dmColor;
        public short dmDuplex;
        public short dmYResolution;
        public short dmTTOption;
        public short dmCollate;

        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)]
        public string dmFormName;

        public short dmLogPixels;
        public int dmBitsPerPel;
        public int dmPelsWidth;
        public int dmPelsHeight;
        public int dmDisplayFlags;
        public int dmDisplayFrequency;
        public int dmICMMethod;
        public int dmICMIntent;
        public int dmMediaType;
        public int dmDitherType;
        public int dmReserved1;
        public int dmReserved2;
        public int dmPanningWidth;
        public int dmPanningHeight;
    }

    public sealed class PixelMetrics
    {
        public int Width;
        public int Height;
        public long PixelCount;
        public double MeanRed;
        public double MeanGreen;
        public double MeanBlue;
        public double MeanLuminance;
        public int MinRed;
        public int MaxRed;
        public int MinGreen;
        public int MaxGreen;
        public int MinBlue;
        public int MaxBlue;
        public double LuminanceVariance;
        public double LuminanceStdDev;
        public int CoarseColorBinsPerChannel;
        public int CoarseColorNonZeroBins;
        public long[] LuminanceHistogram16;
        public double NearWhiteRatio;
        public double NearBlackRatio;
    }

    public sealed class DisplayMonitorInfo
    {
        public string DeviceName;
        public bool IsPrimary;
        public RECT Bounds;
        public RECT WorkingArea;
    }

    public sealed class DisplayDeviceInfo
    {
        public string DeviceName;
        public string DeviceString;
        public int StateFlags;
    }

    public sealed class SystemMetricInfo
    {
        public bool Available;
        public int Value;
        public string Error;
    }

    public sealed class InteractiveDesktopInfo
    {
        public bool Available;
        public string Name;
        public int? ErrorCode;
        public string Error;
        public string NameError;
    }

    public sealed class DesktopCaptureProbe
    {
        public bool Succeeded;
        public bool InvalidHandle;
        public int? ErrorCode;
        public string Error;
    }

    public sealed class DisplayModeInfo
    {
        public string DeviceName;
        public int PositionX;
        public int PositionY;
        public int Width;
        public int Height;
        public int BitsPerPixel;
        public int DisplayFrequency;
        public int DisplayOrientation;
        public int DisplayFlags;
    }

    public static class NativeMethods
    {
        private const uint GW_OWNER = 4;
        private const uint MONITORINFOF_PRIMARY = 1;
        private const uint DESKTOP_READOBJECTS = 0x0001;
        private const int UOI_NAME = 2;
        private const int ERROR_INVALID_HANDLE = 6;
        private delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);
        private delegate bool MonitorEnumProc(
            IntPtr hMonitor,
            IntPtr hdcMonitor,
            IntPtr lprcMonitor,
            IntPtr dwData);

        [DllImport("user32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static extern bool GetWindowRect(IntPtr hWnd, out RECT lpRect);

        [DllImport("user32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdcBlt, uint nFlags);

        [DllImport("user32.dll")]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool EnumWindows(EnumWindowsProc lpEnumFunc, IntPtr lParam);

        [DllImport("user32.dll")]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool IsWindowVisible(IntPtr hWnd);

        [DllImport("user32.dll")]
        private static extern IntPtr GetWindow(IntPtr hWnd, uint uCmd);

        [DllImport("user32.dll")]
        private static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint lpdwProcessId);

        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        private static extern int GetWindowTextLength(IntPtr hWnd);

        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        private static extern int GetWindowText(IntPtr hWnd, StringBuilder lpString, int nMaxCount);

        [DllImport("user32.dll")]
        public static extern int GetSystemMetrics(int nIndex);

        [DllImport("user32.dll", SetLastError = true)]
        public static extern int ChangeDisplaySettings(IntPtr lpDevMode, int dwFlags);

        [DllImport("user32.dll")]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool EnumDisplayMonitors(
            IntPtr hdc,
            IntPtr lprcClip,
            MonitorEnumProc lpfnEnum,
            IntPtr dwData);

        [DllImport("user32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool GetMonitorInfo(IntPtr hMonitor, ref MONITORINFO lpmi);

        [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true, EntryPoint = "GetMonitorInfoW")]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool GetMonitorInfoEx(IntPtr hMonitor, ref MONITORINFOEX lpmi);

        [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool EnumDisplayDevices(
            string lpDevice,
            uint iDevNum,
            ref DISPLAY_DEVICE lpDisplayDevice,
            uint dwFlags);

        [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool EnumDisplaySettings(
            string lpszDeviceName,
            int iModeNum,
            ref DEVMODE lpDevMode);

        [DllImport("user32.dll", SetLastError = true)]
        private static extern IntPtr OpenInputDesktop(uint dwFlags, bool fInherit, uint dwDesiredAccess);

        [DllImport("user32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool CloseDesktop(IntPtr hDesktop);

        [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool GetUserObjectInformation(
            IntPtr hObj,
            int nIndex,
            StringBuilder pvInfo,
            uint nLength,
            out uint lpnLengthNeeded);

        [DllImport("user32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool SetProcessDpiAwarenessContext(IntPtr value);

        [DllImport("user32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool SetProcessDPIAware();

        public static string TryEnablePerMonitorDpiAwareness()
        {
            try
            {
                // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 is -4.
                if (SetProcessDpiAwarenessContext(new IntPtr(-4)))
                {
                    return "per-monitor-v2";
                }
            }
            catch (EntryPointNotFoundException)
            {
                // Windows 8.1 and earlier do not export this entry point.
            }

            try
            {
                if (SetProcessDPIAware())
                {
                    return "system-aware";
                }
            }
            catch (EntryPointNotFoundException)
            {
                // Kept as a best-effort capture improvement.
            }

            return "unchanged";
        }

        public static SystemMetricInfo TryGetSystemMetric(int index)
        {
            SystemMetricInfo result = new SystemMetricInfo();
            try
            {
                result.Available = true;
                result.Value = GetSystemMetrics(index);
            }
            catch (Exception exception)
            {
                result.Available = false;
                result.Error = exception.Message;
            }
            return result;
        }

        public static DisplayMonitorInfo[] GetDisplayMonitors()
        {
            List<DisplayMonitorInfo> monitors = new List<DisplayMonitorInfo>();
            bool enumerated = EnumDisplayMonitors(IntPtr.Zero, IntPtr.Zero,
                delegate(IntPtr hMonitor, IntPtr hdcMonitor, IntPtr lprcMonitor, IntPtr data)
                {
                    MONITORINFOEX monitorInfo = new MONITORINFOEX();
                    monitorInfo.cbSize = Marshal.SizeOf(typeof(MONITORINFOEX));
                    if (GetMonitorInfoEx(hMonitor, ref monitorInfo))
                    {
                        DisplayMonitorInfo info = new DisplayMonitorInfo();
                        info.DeviceName = monitorInfo.szDevice;
                        info.IsPrimary = (monitorInfo.dwFlags & MONITORINFOF_PRIMARY) != 0;
                        info.Bounds = monitorInfo.rcMonitor;
                        info.WorkingArea = monitorInfo.rcWork;
                        monitors.Add(info);
                    }
                    return true;
                }, IntPtr.Zero);

            if (!enumerated)
            {
                int errorCode = Marshal.GetLastWin32Error();
                throw new Win32Exception(errorCode, "EnumDisplayMonitors failed.");
            }

            return monitors.ToArray();
        }

        public static DisplayDeviceInfo[] GetDisplayDevices()
        {
            List<DisplayDeviceInfo> devices = new List<DisplayDeviceInfo>();
            for (uint index = 0; ; index++)
            {
                DISPLAY_DEVICE device = new DISPLAY_DEVICE();
                device.cb = Marshal.SizeOf(typeof(DISPLAY_DEVICE));
                if (!EnumDisplayDevices(null, index, ref device, 0))
                {
                    break;
                }

                DisplayDeviceInfo info = new DisplayDeviceInfo();
                info.DeviceName = device.DeviceName;
                info.DeviceString = device.DeviceString;
                info.StateFlags = device.StateFlags;
                devices.Add(info);
            }
            return devices.ToArray();
        }

        public static DisplayModeInfo GetCurrentDisplayMode(string deviceName)
        {
            if (String.IsNullOrEmpty(deviceName))
            {
                throw new ArgumentException("A display device name is required.", "deviceName");
            }

            DEVMODE mode = new DEVMODE();
            mode.dmSize = (short)Marshal.SizeOf(typeof(DEVMODE));
            const int ENUM_CURRENT_SETTINGS = -1;
            if (!EnumDisplaySettings(deviceName, ENUM_CURRENT_SETTINGS, ref mode))
            {
                int errorCode = Marshal.GetLastWin32Error();
                throw new Win32Exception(errorCode, "EnumDisplaySettings failed.");
            }

            DisplayModeInfo result = new DisplayModeInfo();
            result.DeviceName = deviceName;
            result.PositionX = mode.dmPositionX;
            result.PositionY = mode.dmPositionY;
            result.Width = mode.dmPelsWidth;
            result.Height = mode.dmPelsHeight;
            result.BitsPerPixel = mode.dmBitsPerPel;
            result.DisplayFrequency = mode.dmDisplayFrequency;
            result.DisplayOrientation = mode.dmDisplayOrientation;
            result.DisplayFlags = mode.dmDisplayFlags;
            return result;
        }

        public static InteractiveDesktopInfo GetInteractiveDesktopInfo()
        {
            InteractiveDesktopInfo result = new InteractiveDesktopInfo();
            IntPtr desktop = IntPtr.Zero;
            try
            {
                desktop = OpenInputDesktop(0, false, DESKTOP_READOBJECTS);
                if (desktop == IntPtr.Zero)
                {
                    int errorCode = Marshal.GetLastWin32Error();
                    result.Available = false;
                    result.ErrorCode = errorCode;
                    result.Error = new Win32Exception(errorCode).Message;
                    return result;
                }

                result.Available = true;
                uint required;
                GetUserObjectInformation(desktop, UOI_NAME, null, 0, out required);
                if (required > 0)
                {
                    StringBuilder name = new StringBuilder((int)(required / 2) + 1);
                    if (GetUserObjectInformation(desktop, UOI_NAME, name, required, out required))
                    {
                        result.Name = name.ToString();
                    }
                    else
                    {
                        int errorCode = Marshal.GetLastWin32Error();
                        result.NameError = new Win32Exception(errorCode).Message;
                    }
                }
            }
            catch (Exception exception)
            {
                result.Available = false;
                result.Error = exception.Message;
            }
            finally
            {
                if (desktop != IntPtr.Zero)
                {
                    CloseDesktop(desktop);
                }
            }
            return result;
        }

        public static DesktopCaptureProbe TestDesktopCapture(int sourceX, int sourceY)
        {
            DesktopCaptureProbe result = new DesktopCaptureProbe();
            try
            {
                using (Bitmap bitmap = new Bitmap(1, 1, PixelFormat.Format32bppArgb))
                using (Graphics graphics = Graphics.FromImage(bitmap))
                {
                    graphics.CopyFromScreen(
                        sourceX,
                        sourceY,
                        0,
                        0,
                        new Size(1, 1),
                        CopyPixelOperation.SourceCopy);
                }
                result.Succeeded = true;
            }
            catch (Exception exception)
            {
                int? errorCode = GetNativeErrorCode(exception);
                result.Succeeded = false;
                result.ErrorCode = errorCode;
                result.InvalidHandle = errorCode.HasValue && errorCode.Value == ERROR_INVALID_HANDLE;
                result.Error = exception.Message;
            }
            return result;
        }

        public static int? GetNativeErrorCode(Exception exception)
        {
            Exception current = exception;
            while (current != null)
            {
                Win32Exception win32 = current as Win32Exception;
                if (win32 != null && win32.NativeErrorCode != 0)
                {
                    return win32.NativeErrorCode;
                }

                uint hresult = unchecked((uint)current.HResult);
                if ((hresult & 0xFFFF0000U) == 0x80070000U)
                {
                    return (int)(hresult & 0x0000FFFFU);
                }

                current = current.InnerException;
            }

            int lastError = Marshal.GetLastWin32Error();
            return lastError == ERROR_INVALID_HANDLE ? (int?)lastError : null;
        }

        public static RECT GetPrimaryMonitorBounds()
        {
            DisplayMonitorInfo[] monitors = GetDisplayMonitors();
            foreach (DisplayMonitorInfo monitor in monitors)
            {
                if (monitor.IsPrimary)
                {
                    return monitor.Bounds;
                }
            }

            throw new InvalidOperationException("Could not resolve the primary monitor.");
        }

        public static IntPtr FindVisibleTopLevelWindowForProcess(int processId)
        {
            IntPtr result = IntPtr.Zero;
            uint requestedProcessId = unchecked((uint)processId);
            long largestArea = -1;

            EnumWindows(delegate(IntPtr hWnd, IntPtr ignored)
            {
                uint windowProcessId;
                GetWindowThreadProcessId(hWnd, out windowProcessId);

                if (windowProcessId != requestedProcessId || !IsWindowVisible(hWnd))
                {
                    return true;
                }

                if (GetWindow(hWnd, GW_OWNER) != IntPtr.Zero)
                {
                    return true;
                }

                RECT rect;
                if (!GetWindowRect(hWnd, out rect) || rect.Width <= 0 || rect.Height <= 0)
                {
                    return true;
                }

                long area = (long)rect.Width * (long)rect.Height;
                if (area > largestArea)
                {
                    // Prefer the largest visible unowned top-level window; this
                    // avoids choosing a small auxiliary window over the game.
                    result = hWnd;
                    largestArea = area;
                }
                return true;
            }, IntPtr.Zero);

            return result;
        }

        public static string GetWindowTitle(IntPtr hWnd)
        {
            int length = GetWindowTextLength(hWnd);
            if (length <= 0)
            {
                return String.Empty;
            }

            StringBuilder text = new StringBuilder(length + 1);
            GetWindowText(hWnd, text, text.Capacity);
            return text.ToString();
        }

        public static PixelMetrics AnalyzeImage(
            string path,
            int coarseColorBinsPerChannel,
            int nearWhiteChannel,
            int nearBlackChannel)
        {
            if (coarseColorBinsPerChannel < 2)
            {
                coarseColorBinsPerChannel = 2;
            }
            else if (coarseColorBinsPerChannel > 32)
            {
                coarseColorBinsPerChannel = 32;
            }

            if (nearWhiteChannel < 0) nearWhiteChannel = 0;
            if (nearWhiteChannel > 255) nearWhiteChannel = 255;
            if (nearBlackChannel < 0) nearBlackChannel = 0;
            if (nearBlackChannel > 255) nearBlackChannel = 255;

            using (Bitmap source = new Bitmap(path))
            using (Bitmap bitmap = new Bitmap(source.Width, source.Height, PixelFormat.Format32bppArgb))
            {
                using (Graphics graphics = Graphics.FromImage(bitmap))
                {
                    graphics.DrawImageUnscaled(source, 0, 0);
                }

                Rectangle bounds = new Rectangle(0, 0, bitmap.Width, bitmap.Height);
                BitmapData data = bitmap.LockBits(bounds, ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
                try
                {
                    int absoluteStride = Math.Abs(data.Stride);
                    int bytes = checked(absoluteStride * bitmap.Height);
                    byte[] pixels = new byte[bytes];
                    Marshal.Copy(data.Scan0, pixels, 0, bytes);

                    PixelMetrics metrics = new PixelMetrics();
                    metrics.Width = bitmap.Width;
                    metrics.Height = bitmap.Height;
                    metrics.PixelCount = (long)bitmap.Width * (long)bitmap.Height;
                    metrics.MinRed = 255;
                    metrics.MinGreen = 255;
                    metrics.MinBlue = 255;
                    metrics.MaxRed = 0;
                    metrics.MaxGreen = 0;
                    metrics.MaxBlue = 0;
                    metrics.CoarseColorBinsPerChannel = coarseColorBinsPerChannel;
                    metrics.LuminanceHistogram16 = new long[16];

                    int colorBinCount = checked(
                        coarseColorBinsPerChannel * coarseColorBinsPerChannel * coarseColorBinsPerChannel);
                    bool[] seenColorBins = new bool[colorBinCount];
                    double sumRed = 0.0;
                    double sumGreen = 0.0;
                    double sumBlue = 0.0;
                    double sumLuminance = 0.0;
                    double sumLuminanceSquared = 0.0;
                    long nearWhite = 0;
                    long nearBlack = 0;

                    for (int y = 0; y < bitmap.Height; y++)
                    {
                        int rowOffset = data.Stride >= 0
                            ? y * data.Stride
                            : (bitmap.Height - 1 - y) * absoluteStride;

                        for (int x = 0; x < bitmap.Width; x++)
                        {
                            int offset = rowOffset + x * 4;
                            int blue = pixels[offset];
                            int green = pixels[offset + 1];
                            int red = pixels[offset + 2];

                            if (red < metrics.MinRed) metrics.MinRed = red;
                            if (red > metrics.MaxRed) metrics.MaxRed = red;
                            if (green < metrics.MinGreen) metrics.MinGreen = green;
                            if (green > metrics.MaxGreen) metrics.MaxGreen = green;
                            if (blue < metrics.MinBlue) metrics.MinBlue = blue;
                            if (blue > metrics.MaxBlue) metrics.MaxBlue = blue;

                            sumRed += red;
                            sumGreen += green;
                            sumBlue += blue;

                            double luminance = 0.2126 * red + 0.7152 * green + 0.0722 * blue;
                            sumLuminance += luminance;
                            sumLuminanceSquared += luminance * luminance;

                            int luminanceBin = (int)(luminance * 16.0 / 256.0);
                            if (luminanceBin > 15) luminanceBin = 15;
                            metrics.LuminanceHistogram16[luminanceBin]++;

                            int redBin = red * coarseColorBinsPerChannel / 256;
                            int greenBin = green * coarseColorBinsPerChannel / 256;
                            int blueBin = blue * coarseColorBinsPerChannel / 256;
                            int colorBin = (redBin * coarseColorBinsPerChannel + greenBin)
                                * coarseColorBinsPerChannel + blueBin;
                            if (!seenColorBins[colorBin])
                            {
                                seenColorBins[colorBin] = true;
                                metrics.CoarseColorNonZeroBins++;
                            }

                            if (red >= nearWhiteChannel && green >= nearWhiteChannel && blue >= nearWhiteChannel)
                            {
                                nearWhite++;
                            }

                            if (red <= nearBlackChannel && green <= nearBlackChannel && blue <= nearBlackChannel)
                            {
                                nearBlack++;
                            }
                        }
                    }

                    if (metrics.PixelCount > 0)
                    {
                        metrics.MeanRed = sumRed / metrics.PixelCount;
                        metrics.MeanGreen = sumGreen / metrics.PixelCount;
                        metrics.MeanBlue = sumBlue / metrics.PixelCount;
                        metrics.MeanLuminance = sumLuminance / metrics.PixelCount;
                        double variance = sumLuminanceSquared / metrics.PixelCount
                            - metrics.MeanLuminance * metrics.MeanLuminance;
                        metrics.LuminanceVariance = variance > 0.0 ? variance : 0.0;
                        metrics.LuminanceStdDev = Math.Sqrt(metrics.LuminanceVariance);
                        metrics.NearWhiteRatio = (double)nearWhite / metrics.PixelCount;
                        metrics.NearBlackRatio = (double)nearBlack / metrics.PixelCount;
                    }

                    return metrics;
                }
                finally
                {
                    bitmap.UnlockBits(data);
                }
            }
        }
    }
}
