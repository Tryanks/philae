package a2i.boot;

import android.net.ConnectivityManager;
import android.net.LinkProperties;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.NetworkInfo;
import android.net.NetworkRequest;
import android.os.Bundle;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.Message;
import android.os.Messenger;
import android.os.RemoteException;
import android.util.Log;

/**
 * android.net.IConnectivityManager: which networks exist and what they can do.
 *
 * Apps ask this before they try to reach the internet, and many refuse to try when the
 * answer is "no network". Traffic itself does not go through here: sockets are the host
 * platform's, opened by libcore. So this reports the network the process evidently has,
 * as one connected, validated, unmetered Wi-Fi network. (Telling Wi-Fi from mobile data
 * and noticing when the connection drops would come from the host platform's own
 * reachability monitoring, which is not wired up.)
 */
public final class ConnectivityService {
    private static final String TAG = "a2i.Connectivity";
    private static final int NET_ID = 100;

    private final Network mNetwork = new Network(NET_ID);
    private final NetworkCapabilities mCapabilities = new NetworkCapabilities()
            .addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
            .addCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
            .addCapability(NetworkCapabilities.NET_CAPABILITY_VALIDATED)
            .addCapability(NetworkCapabilities.NET_CAPABILITY_NOT_METERED)
            .addCapability(NetworkCapabilities.NET_CAPABILITY_NOT_ROAMING)
            .addCapability(NetworkCapabilities.NET_CAPABILITY_FOREGROUND)
            .addCapability(NetworkCapabilities.NET_CAPABILITY_NOT_CONGESTED)
            .addCapability(NetworkCapabilities.NET_CAPABILITY_NOT_SUSPENDED)
            .addCapability(NetworkCapabilities.NET_CAPABILITY_NOT_VCN_MANAGED)
            .setLinkDownstreamBandwidthKbps(100_000)
            .setLinkUpstreamBandwidthKbps(50_000);
    private final LinkProperties mLinkProperties = new LinkProperties();
    private final Handler mHandler = new Handler(Looper.getMainLooper());
    private int mNextRequestId = 1;

    ConnectivityService() {
        mLinkProperties.setInterfaceName("wlan0");
        mLinkProperties.setMtu(1500);
    }

    private NetworkInfo networkInfo() {
        NetworkInfo info = new NetworkInfo(ConnectivityManager.TYPE_WIFI, 0, "WIFI", "");
        info.setDetailedState(NetworkInfo.DetailedState.CONNECTED, null, null);
        info.setIsAvailable(true);
        return info;
    }

    private boolean isOurs(Network network) {
        return network != null && network.getNetId() == NET_ID;
    }

    public Network getActiveNetwork() {
        return mNetwork;
    }

    public Network getActiveNetworkForUid(int uid, boolean ignoreBlocked) {
        return mNetwork;
    }

    public NetworkInfo getActiveNetworkInfo() {
        return networkInfo();
    }

    public NetworkInfo getNetworkInfo(int networkType) {
        return networkType == ConnectivityManager.TYPE_WIFI ? networkInfo() : null;
    }

    public NetworkInfo getNetworkInfoForUid(Network network, int uid, boolean ignoreBlocked) {
        return isOurs(network) ? networkInfo() : null;
    }

    public NetworkInfo[] getAllNetworkInfo() {
        return new NetworkInfo[] {networkInfo()};
    }

    public Network[] getAllNetworks() {
        return new Network[] {mNetwork};
    }

    public boolean isNetworkSupported(int networkType) {
        return networkType == ConnectivityManager.TYPE_WIFI;
    }

    public NetworkCapabilities getNetworkCapabilities(Network network, String callingPackage, String featureId) {
        return isOurs(network) ? new NetworkCapabilities(mCapabilities) : null;
    }

    public NetworkCapabilities[] getDefaultNetworkCapabilitiesForUser(int userId, String callingPackage,
            String featureId) {
        return new NetworkCapabilities[] {new NetworkCapabilities(mCapabilities)};
    }

    public LinkProperties getActiveLinkProperties() {
        return new LinkProperties(mLinkProperties);
    }

    public LinkProperties getLinkProperties(Network network) {
        return isOurs(network) ? new LinkProperties(mLinkProperties) : null;
    }

    public boolean isActiveNetworkMetered() {
        return false;
    }

    public boolean isDefaultNetworkActive() {
        return true;
    }

    public int getRestrictBackgroundStatusByCaller() {
        return ConnectivityManager.RESTRICT_BACKGROUND_STATUS_DISABLED;
    }

    /** An app wants a network with certain capabilities, or the default one, and callbacks about it. */
    public NetworkRequest requestNetwork(int asUid, NetworkCapabilities capabilities, int requestType,
            Messenger messenger, int timeoutSec, IBinder binder, int legacyType, int callbackFlags,
            String callingPackage, String callingAttributionTag) {
        NetworkRequest.Type type = NetworkRequest.Type.values()[requestType];
        return register(capabilities, type, legacyType, messenger);
    }

    /** An app wants callbacks about every network with certain capabilities. */
    public NetworkRequest listenForNetwork(NetworkCapabilities capabilities, Messenger messenger, IBinder binder,
            int callbackFlags, String callingPackage, String callingAttributionTag) {
        return register(capabilities, NetworkRequest.Type.LISTEN, ConnectivityManager.TYPE_NONE, messenger);
    }

    public void releaseNetworkRequest(NetworkRequest request) {}

    private synchronized NetworkRequest register(NetworkCapabilities wanted, NetworkRequest.Type type,
            int legacyType, Messenger messenger) {
        NetworkCapabilities capabilities = wanted != null ? wanted : new NetworkCapabilities();
        NetworkRequest request = new NetworkRequest(capabilities, legacyType, mNextRequestId++, type);
        // The one network has been up all along, so the only event there is to deliver is
        // that it is available, if it is the kind of network asked for. It is sent once
        // the caller has had the chance to file its callback under the returned request.
        if (messenger != null && capabilities.satisfiedByNetworkCapabilities(mCapabilities)) {
            mHandler.post(() -> {
                Bundle data = new Bundle();
                data.putParcelable(NetworkRequest.class.getSimpleName(), new NetworkRequest(request));
                data.putParcelable(Network.class.getSimpleName(), mNetwork);
                data.putParcelable(NetworkCapabilities.class.getSimpleName(), new NetworkCapabilities(mCapabilities));
                data.putParcelable(LinkProperties.class.getSimpleName(), new LinkProperties(mLinkProperties));
                Message message = Message.obtain();
                message.what = ConnectivityManager.CALLBACK_AVAILABLE;
                message.arg1 = ConnectivityManager.BLOCKED_REASON_NONE;
                message.setData(data);
                try {
                    messenger.send(message);
                } catch (RemoteException e) {
                    Log.w(TAG, "network callback failed", e);
                }
            });
        }
        return request;
    }
}
