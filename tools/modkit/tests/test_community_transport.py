"""The known-host DNS fallback must never weaken TLS authentication."""
import http.client
from pathlib import Path
import socket
import ssl
import sys
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from community import RepositoryHTTPSConnection, RepositoryHTTPSHandler


class RepositoryTlsTests(unittest.TestCase):
    def test_handler_uses_context_without_removed_constructor_keyword(self):
        handler = RepositoryHTTPSHandler()
        request = Mock()
        with patch.object(handler, "do_open") as opened:
            handler.https_open(request)
        self.assertEqual(opened.call_args.args, (RepositoryHTTPSConnection, request))
        self.assertEqual(set(opened.call_args.kwargs), {"context"})

    def test_dns_fallback_keeps_original_hostname_and_certificate_validation(self):
        context = ssl.create_default_context()
        connection = RepositoryHTTPSConnection("soulmon.fun", context=context)
        raw, wrapped = Mock(), Mock()
        with patch.object(http.client.HTTPSConnection, "connect", side_effect=socket.gaierror()), \
                patch("community.repository_address", return_value="198.51.100.42"), \
                patch("community.socket.create_connection", return_value=raw) as dial, \
                patch.object(context, "wrap_socket", return_value=wrapped) as wrap:
            connection.connect()
        self.assertEqual(dial.call_args.args[0], ("198.51.100.42", 443))
        wrap.assert_called_once_with(raw, server_hostname="soulmon.fun")
        self.assertTrue(context.check_hostname)
        self.assertEqual(context.verify_mode, ssl.CERT_REQUIRED)
        self.assertIs(connection.sock, wrapped)

    def test_missing_private_routing_keeps_dns_failure(self):
        with patch.object(http.client.HTTPSConnection, "connect", side_effect=socket.gaierror()), \
                patch("community.repository_address", return_value=None), \
                patch("community.socket.create_connection") as dial:
            with self.assertRaises(socket.gaierror):
                RepositoryHTTPSConnection("soulmon.fun").connect()
            dial.assert_not_called()

    def test_other_hosts_and_transport_failures_do_not_use_known_server(self):
        for host, failure in (("another.example", socket.gaierror()),
                              ("soulmon.fun", TimeoutError()),
                              ("soulmon.fun", ssl.SSLCertVerificationError())):
            with self.subTest(host=host, failure=type(failure)), \
                    patch.object(http.client.HTTPSConnection, "connect", side_effect=failure), \
                    patch("community.socket.create_connection") as dial:
                with self.assertRaises(type(failure)):
                    RepositoryHTTPSConnection(host).connect()
                dial.assert_not_called()

    def test_invalid_fallback_certificate_is_rejected_and_socket_closed(self):
        context = ssl.create_default_context()
        connection = RepositoryHTTPSConnection("soulmon.fun", context=context)
        raw = Mock()
        with patch.object(http.client.HTTPSConnection, "connect", side_effect=socket.gaierror()), \
                patch("community.repository_address", return_value="198.51.100.42"), \
                patch("community.socket.create_connection", return_value=raw), \
                patch.object(context, "wrap_socket", side_effect=ssl.SSLCertVerificationError()):
            with self.assertRaises(ssl.SSLCertVerificationError):
                connection.connect()
        raw.close.assert_called_once()


if __name__ == "__main__":
    unittest.main()
