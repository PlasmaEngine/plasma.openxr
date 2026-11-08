using System;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Net.Http;
using PlasmaBuild.Core.Configuration;
using PlasmaBuild.Core.Rules;

public static class PlasmaBuildOpenXR
{
    public const string Version = "1.1.47";
    private const string PackageName = "OpenXR." + Version + ".nupkg";
    private const string DownloadUrl = "https://www.nuget.org/api/v2/package/OpenXR/" + Version;

    // Must match AgilitySdkModule.Version in the engine: RendererDX12's headers are built against it.
    public const string AgilitySdkVersion = "1.619.5";
    private const string AgilitySdkPackageName = "microsoft.direct3d.d3d12." + AgilitySdkVersion + ".nupkg";
    private const string AgilitySdkDownloadUrl =
        "https://api.nuget.org/v3-flatcontainer/microsoft.direct3d.d3d12/" + AgilitySdkVersion + "/" + AgilitySdkPackageName;

    public static bool IsSupported(BuildContext context, out string disabledReason)
    {
        disabledReason = string.Empty;

        return true;
    }

    public static string GetOpenXRPath(BuildContext context)
    {
        return Path.Combine(GetThirdPartyRoot(context), "OpenXR-" + Version);
    }

    public static void EnsureOpenXR(BuildContext context)
    {
        if (!IsSupported(context, out var disabledReason))
        {
            throw new InvalidOperationException(disabledReason);
        }

        EnsureNuGetPackage(context, PackageName, DownloadUrl, GetOpenXRPath(context), HasExpectedOpenXRFiles);
    }

    /// \brief The package-local Agility SDK include directory, downloaded if missing.
    ///
    /// Used when the engine SDK does not carry its own copy, e.g. an installed engine without Intermediate/.
    public static string EnsureAgilitySdkInclude(BuildContext context)
    {
        var sdkPath = Path.Combine(GetThirdPartyRoot(context), "AgilitySDK-" + AgilitySdkVersion);
        EnsureNuGetPackage(context, AgilitySdkPackageName, AgilitySdkDownloadUrl, sdkPath, HasExpectedAgilitySdkFiles);
        return GetAgilitySdkInclude(sdkPath);
    }

    public static string GetAgilitySdkInclude(string sdkPath)
    {
        return Path.Combine(sdkPath, "build", "native", "include");
    }

    public static bool HasExpectedAgilitySdkFiles(string sdkPath)
    {
        return File.Exists(Path.Combine(GetAgilitySdkInclude(sdkPath), "d3d12.h"));
    }

    private static string GetThirdPartyRoot(BuildContext context)
    {
        return Path.Combine(context.ProjectRoot, "Intermediate", "PlasmaBuild", "ThirdParty");
    }

    private static bool HasExpectedOpenXRFiles(string openXrPath)
    {
        return File.Exists(Path.Combine(openXrPath, "include", "openxr", "openxr.h")) &&
            File.Exists(Path.Combine(openXrPath, "native", "x64", "lib", "openxr_loader.lib")) &&
            File.Exists(Path.Combine(openXrPath, "native", "x64", "bin", "openxr_loader.dll"));
    }

    /// \brief Downloads a NuGet package into Intermediate/PlasmaBuild/ThirdParty and extracts it to extractPath.
    ///
    /// A cached archive that fails to extract is treated as a broken download and fetched again once.
    private static void EnsureNuGetPackage(BuildContext context, string packageName, string url, string extractPath,
        Func<string, bool> hasExpectedFiles)
    {
        if (hasExpectedFiles(extractPath))
        {
            return;
        }

        var archivePath = Path.Combine(GetThirdPartyRoot(context), packageName);
        Directory.CreateDirectory(extractPath);

        if (!File.Exists(archivePath))
        {
            DownloadFile(url, archivePath);
        }

        try
        {
            ZipFile.ExtractToDirectory(archivePath, extractPath, overwriteFiles: true);
        }
        catch (InvalidDataException)
        {
            File.Delete(archivePath);
            DownloadFile(url, archivePath);
            ZipFile.ExtractToDirectory(archivePath, extractPath, overwriteFiles: true);
        }

        if (!hasExpectedFiles(extractPath))
        {
            throw new InvalidOperationException(
                $"Extracting '{packageName}' finished, but '{extractPath}' does not contain the expected files.");
        }
    }

    private static void DownloadFile(string url, string targetPath)
    {
        using var httpClient = new HttpClient();
        using var response = httpClient.GetAsync(url, HttpCompletionOption.ResponseHeadersRead).GetAwaiter().GetResult();
        response.EnsureSuccessStatusCode();

        Directory.CreateDirectory(Path.GetDirectoryName(targetPath)!);

        // Written beside the target and moved into place, so an interrupted download never looks like a cached archive.
        var tempPath = targetPath + ".partial";
        try
        {
            using (var source = response.Content.ReadAsStreamAsync().GetAwaiter().GetResult())
            using (var target = File.Create(tempPath))
            {
                source.CopyTo(target);
            }

            File.Move(tempPath, targetPath, overwrite: true);
        }
        finally
        {
            if (File.Exists(tempPath))
            {
                File.Delete(tempPath);
            }
        }
    }
}

public class OpenXRPluginModule : ModuleRules
{
    public OpenXRPluginModule(BuildContext context) : base(context)
    {
        PlasmaBuildOpenXR.EnsureOpenXR(context);
        var openXrPath = PlasmaBuildOpenXR.GetOpenXRPath(context);

        PlasmaPackageSdk.ConfigurePackageModule(this, context, "Source/OpenXRPlugin", "OpenXRPluginPCH.h",
            "BUILDSYSTEM_BUILDING_OPENXRPLUGIN_LIB");

        // Vulkan and Direct3D 12 headers stay in the engine - the renderers need them - so they are read
        // out of the SDK rather than vendored here. The session binds to whichever renderer is active.
        PublicIncludePaths.Add(Path.Combine(PlasmaPackageSdk.Root, "Code", "ThirdParty", "Vulkan-Headers", "include"));
        PrivateLibraries.Add(Path.Combine(PlasmaPackageSdk.SdkBinaryDirectory(context), "plRendererVulkan.lib"));
        PublicIncludePaths.Add(FindAgilitySdkInclude(context));
        PrivateLibraries.Add(Path.Combine(PlasmaPackageSdk.SdkBinaryDirectory(context), "plRendererDX12.lib"));

        PublicIncludePaths.Add(Path.Combine(openXrPath, "include"));
        PublicLibraries.Add(Path.Combine(openXrPath, "native", "x64", "lib", "openxr_loader.lib"));
        PublicDefinitions.Add("XR_USE_GRAPHICS_API_VULKAN");

        // The OpenXR loader ships with the package: it is Apache-2.0, and the plugin cannot load
        // without it.
        StageLoader(context, openXrPath);
    }

    /// \brief The Agility SDK include directory the engine's RendererDX12 headers were built against.
    ///
    /// RendererDX12 uses interfaces (ID3D12Device10, ID3D12GraphicsCommandList7) that the Windows SDK copy of
    /// d3d12.h may lack, so the Agility headers must come first. The engine SDK's own copy wins; an SDK without one
    /// (an installed engine ships no Intermediate/) falls back to the pinned version, downloaded into this package.
    private static string FindAgilitySdkInclude(BuildContext context)
    {
        var thirdParty = Path.Combine(PlasmaPackageSdk.Root, "Intermediate", "PlasmaBuild", "ThirdParty");
        if (Directory.Exists(thirdParty))
        {
            foreach (var dir in Directory.GetDirectories(thirdParty, "AgilitySDK-*").OrderByDescending(d => d, StringComparer.OrdinalIgnoreCase))
            {
                if (PlasmaBuildOpenXR.HasExpectedAgilitySdkFiles(dir))
                {
                    return PlasmaBuildOpenXR.GetAgilitySdkInclude(dir);
                }
            }
        }

        return PlasmaBuildOpenXR.EnsureAgilitySdkInclude(context);
    }

    /// \brief Copies openxr_loader.dll and its licence beside the plugin.
    ///
    /// The engine does this through PlasmaBuildRuntimeFiles, which does not exist out here.
    private static void StageLoader(BuildContext context, string openXrPath)
    {
        var target = PlasmaPackageSdk.PackageBinaryDirectory(context);
        Copy(Path.Combine(openXrPath, "native", "x64", "bin", "openxr_loader.dll"),
            Path.Combine(target, "openxr_loader.dll"));

        // The NuGet package declares Apache-2.0 as an SPDX expression and carries no licence text,
        // so the package supplies it. Staged beside the DLL and listed in PackageDependencies, which
        // puts both in the same component - a component is what gets unpacked as a unit.
        Copy(Path.Combine(context.ProjectRoot, "ThirdParty", "OpenXR", "LICENSE.txt"),
            Path.Combine(target, "openxr-LICENSE.txt"));
    }

    private static void Copy(string source, string target)
    {
        if (!File.Exists(source))
        {
            throw new FileNotFoundException($"Missing OpenXR file '{source}'.", source);
        }

        if (File.Exists(target))
        {
            var from = new FileInfo(source);
            var to = new FileInfo(target);

            if (from.Length == to.Length && from.LastWriteTimeUtc <= to.LastWriteTimeUtc)
            {
                return;
            }
        }

        Directory.CreateDirectory(Path.GetDirectoryName(target));
        File.Copy(source, target, overwrite: true);
    }
}
