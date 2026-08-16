{
  description = "Bonded Inbox Logos Core module";

  nixConfig = {
    extra-substituters = [ "https://cache.nix.logos.co/public" ];
    extra-trusted-public-keys = [
      "public:l4HrXgL4nw246+LBh2SOJyhz64BoGegOYLheT/iIAPU="
    ];
  };

  inputs = {
    logos-module-builder.url =
      "github:logos-co/logos-module-builder/7fbb9420a3fe8ce03a140f0df84a0cc8463bc6dc";
    delivery_module = {
      url = "github:logos-co/logos-delivery-module/3f0f2d8b202f427a96179407bbf18b449935da7c";
      inputs.logos-module-builder.follows = "logos-module-builder";
    };
    storage_module = {
      url = "github:logos-co/logos-storage-module/e0db835de379f47bae7fccc3032056d99af973bb";
      inputs.logos-module-builder.follows = "logos-module-builder";
    };
  };

  outputs = inputs@{ logos-module-builder, ... }:
    let
      core = logos-module-builder.lib.mkLogosModule {
        src = ./.;
        configFile = ./metadata.json;
        flakeInputs = inputs;
      };
      basecampInputs = inputs // {
        bonded_inbox = core // { inherit inputs; };
      };
      basecamp = logos-module-builder.lib.mkLogosQmlModule {
        src = ./basecamp;
        configFile = ./basecamp/metadata.json;
        flakeInputs = basecampInputs;
      };
    in
    core // {
      packages = builtins.mapAttrs
        (system: packages:
          packages // {
            basecamp = basecamp.packages.${system}.default;
            basecamp-lgx = basecamp.packages.${system}.lgx;
          })
        core.packages;
      apps = builtins.mapAttrs
        (system: _:
          let
            pkgs = logos-module-builder.inputs.nixpkgs.legacyPackages.${system};
            qml = pkgs.qt6.qtdeclarative;
            preview = pkgs.writeShellScript "bonded-basecamp-preview" ''
              exec ${qml}/bin/qmlscene \
                -I ${qml}/lib/qt-6/qml \
                -I ${./basecamp} \
                ${./basecamp/preview/Main.qml}
            '';
            smoke = pkgs.writeShellScript "bonded-basecamp-preview-smoke" ''
              export QT_QPA_PLATFORM=offscreen
              export QT_ACCESSIBILITY=0
              exec ${qml}/bin/qmlscene \
                -I ${qml}/lib/qt-6/qml \
                -I ${./basecamp} \
                ${./basecamp/preview/Smoke.qml}
            '';
          in
          (core.apps.${system} or { }) // {
            basecamp = basecamp.apps.${system}.default;
            basecamp-preview = {
              type = "app";
              program = "${preview}";
            };
            basecamp-preview-smoke = {
              type = "app";
              program = "${smoke}";
            };
          })
        core.packages;
    };
}
