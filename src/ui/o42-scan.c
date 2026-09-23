/* o42-scan.c - see o42-scan.h
 *
 * Copyright (C) 2026 The office42 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "o42-scan.h"
#include "o42-image.h"

#include <glib/gstdio.h>
#include <string.h>

#ifdef G_OS_WIN32
#define COBJMACROS
#include <windows.h>
#include <objbase.h>
#include <oaidl.h>
#endif

/* The bytes of a file the scan was saved to, sized and named. */
static GBytes *
take_file (const char *path, const char **format, int *width, int *height, GError **error)
{
  GFile *file = g_file_new_for_path (path);
  GBytes *bytes = o42_image_load_file (file, width, height, format, error);

  g_object_unref (file);
  g_unlink (path);
  return bytes;
}

#ifdef G_OS_WIN32

/* One late-bound call on a COM automation object, by name: what
 * Visual Basic does with a WIA.CommonDialog.  The arguments are given
 * in the order the method lists them; IDispatch wants them reversed. */
static HRESULT
invoke (IDispatch *object, const wchar_t *method, VARIANT *args, int n_args, VARIANT *result)
{
  DISPID id;
  DISPPARAMS params;
  VARIANT *reversed = g_new0 (VARIANT, MAX (n_args, 1));
  HRESULT hr;
  wchar_t *name = (wchar_t *) method;

  hr = IDispatch_GetIDsOfNames (object, &IID_NULL, &name, 1, LOCALE_USER_DEFAULT, &id);
  if (FAILED (hr))
    {
      g_free (reversed);
      return hr;
    }
  for (int i = 0; i < n_args; i++)
    reversed[i] = args[n_args - 1 - i];
  params.rgvarg = reversed;
  params.cArgs = n_args;
  params.rgdispidNamedArgs = NULL;
  params.cNamedArgs = 0;
  if (result != NULL)
    VariantInit (result);
  hr = IDispatch_Invoke (object, id, &IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_METHOD,
                         &params, result, NULL, NULL);
  g_free (reversed);
  return hr;
}

gboolean
o42_scan_available (void)
{
  return TRUE;
}

GBytes *
o42_scan_acquire (char **format, int *width, int *height, GError **error)
{
  CLSID clsid;
  IDispatch *dialog = NULL;
  VARIANT args[7], image;
  HRESULT hr;
  GBytes *bytes = NULL;
  const char *fmt = NULL;
  gboolean inited;

  hr = CoInitializeEx (NULL, COINIT_APARTMENTTHREADED);
  inited = SUCCEEDED (hr);
  if (FAILED (CLSIDFromProgID (L"WIA.CommonDialog", &clsid)) ||
      FAILED (CoCreateInstance (&clsid, NULL, CLSCTX_INPROC_SERVER, &IID_IDispatch, (void **) &dialog)))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                           "Windows Image Acquisition is not available on this computer.");
      if (inited) CoUninitialize ();
      return NULL;
    }

  /* ShowAcquireImage (DeviceType, Intent, Bias, FormatID, AlwaysSelectDevice,
   * UseCommonUI, CancelError): any device, as it is, as PNG, the
   * device chosen when there is more than one, the system's own
   * dialog, and a cancel coming back as no picture. */
  for (int i = 0; i < 7; i++)
    VariantInit (&args[i]);
  args[0].vt = VT_I4; args[0].lVal = 0;          /* UnspecifiedDeviceType */
  args[1].vt = VT_I4; args[1].lVal = 0;          /* UnspecifiedIntent */
  args[2].vt = VT_I4; args[2].lVal = 131072;     /* MaximizeQuality */
  args[3].vt = VT_BSTR;
  args[3].bstrVal = SysAllocString (L"{B96B3CAF-0728-11D3-9D7B-0000F81EF32E}");
  args[4].vt = VT_BOOL; args[4].boolVal = VARIANT_FALSE;
  args[5].vt = VT_BOOL; args[5].boolVal = VARIANT_TRUE;
  args[6].vt = VT_BOOL; args[6].boolVal = VARIANT_FALSE;

  hr = invoke (dialog, L"ShowAcquireImage", args, 7, &image);
  SysFreeString (args[3].bstrVal);
  if (SUCCEEDED (hr) && image.vt == VT_DISPATCH && image.pdispVal != NULL)
    {
      /* ImageFile.SaveFile (path): to a temporary file, read back. */
      char *path = NULL;
      int fd = g_file_open_tmp ("office42-scan-XXXXXX.png", &path, error);

      if (fd >= 0)
        {
          VARIANT arg, unused;
          wchar_t *wpath = g_utf8_to_utf16 (path, -1, NULL, NULL, NULL);

          g_close (fd, NULL);
          g_unlink (path);
          VariantInit (&arg);
          arg.vt = VT_BSTR;
          arg.bstrVal = SysAllocString (wpath);
          hr = invoke (image.pdispVal, L"SaveFile", &arg, 1, &unused);
          SysFreeString (arg.bstrVal);
          g_free (wpath);
          if (SUCCEEDED (hr))
            bytes = take_file (path, &fmt, width, height, error);
          else
            g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                         "The picture could not be saved (0x%08lx).", (unsigned long) hr);
          g_free (path);
        }
      IDispatch_Release (image.pdispVal);
    }
  else if (FAILED (hr) && (unsigned long) hr != 0x80210015UL /* WIA_S_NO_DEVICE_AVAILABLE */ &&
           (unsigned long) hr != 0x80070057UL /* E_INVALIDARG: no device */)
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                 "No picture came from the scanner or camera (0x%08lx).", (unsigned long) hr);
  else if (FAILED (hr))
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                         "No scanner or camera is connected.");

  IDispatch_Release (dialog);
  if (inited)
    CoUninitialize ();
  if (format != NULL)
    *format = g_strdup (fmt != NULL ? fmt : "png");
  return bytes;
}

#else  /* not Windows: SANE's scanimage */

gboolean
o42_scan_available (void)
{
  char *path = g_find_program_in_path ("scanimage");
  gboolean found = path != NULL;

  g_free (path);
  return found;
}

GBytes *
o42_scan_acquire (char **format, int *width, int *height, GError **error)
{
  char *path = NULL;
  int fd = g_file_open_tmp ("office42-scan-XXXXXX.png", &path, error);
  char *stderr_text = NULL;
  int status = 0;
  GBytes *bytes = NULL;
  const char *fmt = NULL;

  if (fd < 0)
    return NULL;
  g_close (fd, NULL);
  {
    const char *argv[] = { "scanimage", "--format=png", "-o", path, NULL };

    if (!g_spawn_sync (NULL, (char **) argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL,
                       NULL, NULL, NULL, &stderr_text, &status, error))
      {
        g_unlink (path);
        g_free (path);
        return NULL;
      }
  }
  if (!g_spawn_check_wait_status (status, NULL))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "scanimage could not scan: %s",
                   stderr_text != NULL && *stderr_text != '\0' ? g_strstrip (stderr_text)
                                                               : "is a scanner connected?");
      g_unlink (path);
    }
  else
    bytes = take_file (path, &fmt, width, height, error);
  g_free (stderr_text);
  g_free (path);
  if (format != NULL)
    *format = g_strdup (fmt != NULL ? fmt : "png");
  return bytes;
}

#endif
