{ pkgs ? import <nixpkgs> { } }:

with pkgs;

stdenv.mkDerivation rec {
  pname = "folve";
  version = "1.0";

  src = fetchFromGitHub {
    owner = "hzeller";
    repo = "folve";
    rev = "v${version}";
    hash = "sha256-msZWZ5KGq0wGacYO/4j3VqihcdgnlO1MX2aS+F27wdE=";
  };

  nativeBuildInputs = [ pkg-config ];
  buildInputs = [
      zita-convolver fftwFloat
      flac
      fuse3
      libmicrohttpd
      libsndfile
  ];

  buildFlags = [ "PREFIX=$(out)" "F_VERSION=${version}"];
  installFlags = [ "PREFIX=$(out)" ];
}
