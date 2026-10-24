using System;
using System.Runtime.InteropServices;

public static class WU2 {
  [StructLayout(LayoutKind.Sequential)]
  public struct IFDESC { public byte bLength; public byte bDescriptorType; public byte bInterfaceNumber; public byte bAlternateSetting; public byte bNumEndpoints; public byte bInterfaceClass; public byte bInterfaceSubClass; public byte bInterfaceProtocol; public byte iInterface; }
  [StructLayout(LayoutKind.Sequential)]
  public struct PIPEINFO { public int PipeType; public byte PipeId; public ushort MaximumPacketSize; public byte Interval; }
  [DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)]
  public static extern IntPtr CreateFileW(string name, uint access, uint share, IntPtr sec, uint disp, uint flags, IntPtr tmpl);
  [DllImport("winusb.dll", SetLastError=true)]
  public static extern bool WinUsb_Initialize(IntPtr handle, out IntPtr wh);
  [DllImport("winusb.dll", SetLastError=true)]
  public static extern bool WinUsb_QueryInterfaceSettings(IntPtr wh, byte alt, out IFDESC desc);
  [DllImport("winusb.dll", SetLastError=true)]
  public static extern bool WinUsb_QueryPipe(IntPtr wh, byte alt, byte idx, out PIPEINFO info);
  [DllImport("winusb.dll", SetLastError=true)]
  public static extern bool WinUsb_SetPipePolicy(IntPtr wh, uint pid, uint type, uint len, ref uint val);
  [DllImport("winusb.dll", SetLastError=true)]
  public static extern bool WinUsb_WritePipe(IntPtr wh, byte pid, byte[] buf, uint len, out uint written, IntPtr ov);
  [DllImport("winusb.dll", SetLastError=true)]
  public static extern bool WinUsb_ReadPipe(IntPtr wh, byte pid, byte[] buf, uint len, out uint read, IntPtr ov);
  [DllImport("winusb.dll", SetLastError=true)]
  public static extern bool WinUsb_ResetPipe(IntPtr wh, byte pid);
  [DllImport("winusb.dll", SetLastError=true)]
  public static extern bool WinUsb_AbortPipe(IntPtr wh, byte pid);
  [DllImport("winusb.dll", SetLastError=true)]
  public static extern bool WinUsb_FlushPipe(IntPtr wh, byte pid);
  [DllImport("winusb.dll", SetLastError=true)]
  public static extern bool WinUsb_Free(IntPtr wh);
  [DllImport("kernel32.dll", SetLastError=true)]
  public static extern bool CloseHandle(IntPtr h);
}
