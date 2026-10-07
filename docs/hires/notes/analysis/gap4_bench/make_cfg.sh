#!/bin/bash
# usage: make_cfg.sh name gw gh dw dh scale scaler
G=/home/simonea/ultima7_exult/tmp/gap4
N=$1; GW=$2; GH=$3; DW=$4; DH=$5; SC=$6; SCL=$7
mkdir -p $G/gamedat_$N
cat > $G/cfg/$N.cfg <<XML
<config>
  <disk>
    <data_path>$G/exult-src/data</data_path>
    <game>
      <blackgate>
        <path>$G/u7bg</path>
        <static_path>/mnt/e/Games/RolePlayingGames/ultima7/static</static_path>
        <patch>$G/patch</patch>
        <mods>$G/mods</mods>
        <savegame_path>$G/save</savegame_path>
        <gamedat_path>$G/gamedat_$N</gamedat_path>
        <keys>(default)</keys>
      </blackgate>
    </game>
  </disk>
  <gameplay>
    <skip_intro>yes</skip_intro>
    <skip_splash>yes</skip_splash>
    <smooth_scrolling>0</smooth_scrolling>
    <enhancements>yes</enhancements>
    <bg_paperdolls>yes</bg_paperdolls>
  </gameplay>
  <audio>
    <enabled>no</enabled>
    <midi><enabled>no</enabled></midi>
  </audio>
  <video>
    <fullscreen>no</fullscreen>
    <vsync>0</vsync>
    <share_video_settings>yes</share_video_settings>
    <display><width>$DW</width><height>$DH</height></display>
    <game><width>$GW</width><height>$GH</height></game>
    <scale>$SC</scale>
    <scale_method>$SCL</scale_method>
    <fill_mode>Fit</fill_mode>
    <fill_scaler>point</fill_scaler>
  </video>
  <shortcutbar><use_shortcutbar>no</use_shortcutbar></shortcutbar>
</config>
XML
echo $G/cfg/$N.cfg
